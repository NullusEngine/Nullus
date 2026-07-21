#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <atomic>
#include <type_traits>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Guid.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/NativeArtifactContainer.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Geometry/BoundingSphereUtils.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"

namespace
{
    std::string WithTrailingSlash(const std::filesystem::path& path)
    {
        auto text = path.string();
        if (!text.empty() && text.back() != '/' && text.back() != '\\')
            text.push_back('/');
        return text;
    }

    std::filesystem::path BuiltinMeshArtifactPath(
        const std::filesystem::path& assetRoot,
        const std::string& virtualSourcePath)
    {
        return assetRoot /
            "Library" /
            "BuiltinArtifacts" /
            "Models" /
            NLS::Core::Assets::BuildArtifactStorageFileName("BuiltinMeshArtifact:" + virtualSourcePath);
    }

    NLS::Render::Context::Driver& EnsureMeshTestDriver()
    {
        static auto driver = std::make_unique<NLS::Render::Context::Driver>([]()
        {
            NLS::Render::Settings::DriverSettings settings;
            settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
            settings.enableExplicitRHI = false;
            return settings;
        }());
        NLS::Core::ServiceLocator::Provide(*driver);
        return *driver;
    }

    NLS::Render::Geometry::Vertex VertexAt(float x, float y, float z)
    {
        NLS::Render::Geometry::Vertex vertex{};
        vertex.position[0] = x;
        vertex.position[1] = y;
        vertex.position[2] = z;
        return vertex;
    }

    NLS::Render::Assets::MeshArtifactData GridMeshArtifact(
        const uint32_t sideVertexCount,
        const float xOffset = 0.0f)
    {
        NLS::Render::Assets::MeshArtifactData mesh;
        mesh.materialIndex = 7u;
        mesh.vertices.reserve(static_cast<size_t>(sideVertexCount) * sideVertexCount);
        for (uint32_t z = 0u; z < sideVertexCount; ++z)
        {
            for (uint32_t x = 0u; x < sideVertexCount; ++x)
            {
                const auto denominator = static_cast<float>(sideVertexCount - 1u);
                mesh.vertices.push_back(VertexAt(
                    xOffset - 1.0f + 2.0f * static_cast<float>(x) / denominator,
                    0.0f,
                    -1.0f + 2.0f * static_cast<float>(z) / denominator));
            }
        }
        for (uint32_t z = 0u; z + 1u < sideVertexCount; ++z)
        {
            for (uint32_t x = 0u; x + 1u < sideVertexCount; ++x)
            {
                const auto topLeft = z * sideVertexCount + x;
                const auto topRight = topLeft + 1u;
                const auto bottomLeft = topLeft + sideVertexCount;
                const auto bottomRight = bottomLeft + 1u;
                mesh.indices.insert(mesh.indices.end(), {
                    topLeft, bottomLeft, topRight,
                    topRight, bottomLeft, bottomRight
                });
            }
        }
        return mesh;
    }

    void AppendUInt32(std::vector<uint8_t>& bytes, uint32_t value)
    {
        bytes.push_back(static_cast<uint8_t>(value & 0xFFu));
        bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
        bytes.push_back(static_cast<uint8_t>((value >> 16u) & 0xFFu));
        bytes.push_back(static_cast<uint8_t>((value >> 24u) & 0xFFu));
    }

    std::vector<uint8_t> BuildLegacyV1MeshArtifactBytes(
        const std::vector<NLS::Render::Geometry::Vertex>& vertices,
        const std::vector<uint32_t>& indices,
        uint32_t materialIndex)
    {
        std::vector<uint8_t> bytes;
        AppendUInt32(bytes, 0x484D4E4Eu);
        AppendUInt32(bytes, 1u);
        AppendUInt32(bytes, static_cast<uint32_t>(sizeof(NLS::Render::Geometry::Vertex)));
        AppendUInt32(bytes, static_cast<uint32_t>(vertices.size()));
        AppendUInt32(bytes, static_cast<uint32_t>(indices.size()));
        AppendUInt32(bytes, materialIndex);
        if (!vertices.empty())
        {
            const auto* begin = reinterpret_cast<const uint8_t*>(vertices.data());
            bytes.insert(bytes.end(), begin, begin + vertices.size() * sizeof(NLS::Render::Geometry::Vertex));
        }
        if (!indices.empty())
        {
            const auto* begin = reinterpret_cast<const uint8_t*>(indices.data());
            bytes.insert(bytes.end(), begin, begin + indices.size() * sizeof(uint32_t));
        }
        return bytes;
    }

    std::vector<uint8_t> WrapMeshArtifactPayload(std::vector<uint8_t> payload)
    {
        NLS::Core::Assets::NativeArtifactMetadata metadata;
        metadata.artifactType = NLS::Core::Assets::ArtifactType::Mesh;
        metadata.schemaName = "mesh";
        metadata.schemaVersion = 3u;
        return NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), payload);
    }

    std::vector<uint8_t> ReadMeshArtifactPayload(const std::vector<uint8_t>& bytes)
    {
        const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(
            bytes,
            NLS::Core::Assets::ArtifactType::Mesh,
            3u);
        if (!container.has_value())
            return {};

        return container->payload;
    }

    void WriteMeshArtifact(
        const std::filesystem::path& path,
        const std::vector<NLS::Render::Geometry::Vertex>& vertices,
        const std::vector<uint32_t>& indices,
        uint32_t materialIndex)
    {
        const auto bytes = NLS::Render::Assets::SerializeMeshArtifact({vertices, indices, materialIndex});
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
}

TEST(MeshBoundingSphereTests, ComputesCenterFromAllNegativeVertexPositions)
{
    EnsureMeshTestDriver();

    const std::vector<NLS::Render::Geometry::Vertex> vertices{
        VertexAt(-8.0f, -6.0f, -4.0f),
        VertexAt(-2.0f, -10.0f, -12.0f),
        VertexAt(-5.0f, -3.0f, -7.0f)
    };
    const std::vector<uint32_t> indices{ 0u, 1u, 2u };

    const NLS::Render::Resources::Mesh mesh(vertices, indices, 0u);
    const auto& sphere = mesh.GetBoundingSphere();

    EXPECT_FLOAT_EQ(sphere.position.x, -5.0f);
    EXPECT_FLOAT_EQ(sphere.position.y, -6.5f);
    EXPECT_FLOAT_EQ(sphere.position.z, -8.0f);
}

TEST(MeshBoundsTests, ComputesLocalAABBFromVertexPositions)
{
    EnsureMeshTestDriver();

    const std::vector<NLS::Render::Geometry::Vertex> vertices{
        VertexAt(-8.0f, -6.0f, -4.0f),
        VertexAt(-2.0f, -10.0f, -12.0f),
        VertexAt(-5.0f, -3.0f, -7.0f)
    };
    const std::vector<uint32_t> indices{ 0u, 1u, 2u };

    const NLS::Render::Resources::Mesh mesh(vertices, indices, 0u);
    const auto& bounds = mesh.GetBounds();

    EXPECT_FLOAT_EQ(bounds.center.x, -5.0f);
    EXPECT_FLOAT_EQ(bounds.center.y, -6.5f);
    EXPECT_FLOAT_EQ(bounds.center.z, -8.0f);
    EXPECT_FLOAT_EQ(bounds.size.x, 6.0f);
    EXPECT_FLOAT_EQ(bounds.size.y, 7.0f);
    EXPECT_FLOAT_EQ(bounds.size.z, 8.0f);
}

TEST(MeshBoundingSphereTests, LoadMeshArtifactHonorsCancellationBeforeReading)
{
    const auto root = std::filesystem::temp_directory_path() / ("nullus_mesh_cancel_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    const auto artifactPath = root / "d69580ece9e4b3f38afa942912e896a556fb2945c91aa8b25e8a09440cd8d5c2";

    const std::vector<NLS::Render::Geometry::Vertex> vertices{
        VertexAt(-1.0f, 0.0f, 0.0f),
        VertexAt(1.0f, 0.0f, 0.0f),
        VertexAt(0.0f, 1.0f, 0.0f)
    };
    WriteMeshArtifact(artifactPath, vertices, {0u, 1u, 2u}, 0u);

    std::atomic_bool cancelled{true};
    EXPECT_FALSE(NLS::Render::Assets::LoadMeshArtifact(artifactPath, &cancelled).has_value());

    std::filesystem::remove_all(root);
}

TEST(MeshArtifactTests, ContentAddressedLoadReadsPayloadDirectlyIntoFinalVectors)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_mesh_direct_payload_" + NLS::Guid::New().ToString());
    const auto artifactPath = root / "Library" / "Artifacts" / "ab" /
        "ab0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcd";
    std::filesystem::create_directories(artifactPath.parent_path());

    const std::vector<NLS::Render::Geometry::Vertex> vertices {
        VertexAt(-3.0f, 1.0f, 2.0f),
        VertexAt(4.0f, 5.0f, 6.0f),
        VertexAt(7.0f, 8.0f, 9.0f)
    };
    const std::vector<uint32_t> indices {2u, 0u, 1u};
    WriteMeshArtifact(artifactPath, vertices, indices, 17u);

    const auto portablePath =
        NLS::Core::Assets::TryMakePortableContentArtifactPath(artifactPath.generic_string());
    ASSERT_FALSE(portablePath.empty());
    NLS::Core::Assets::RegisterRuntimeAuthorizedArtifactPath(portablePath);
    NLS::Core::Assets::SetArtifactLoadTelemetryEnabled(true);
    NLS::Core::Assets::ClearArtifactLoadTelemetry();

    const auto artifact = NLS::Render::Assets::LoadMeshArtifact(artifactPath);

    ASSERT_TRUE(artifact.has_value());
    ASSERT_EQ(artifact->vertices.size(), vertices.size());
    EXPECT_EQ(artifact->indices, indices);
    EXPECT_EQ(artifact->materialIndex, 17u);
    EXPECT_FLOAT_EQ(artifact->vertices[0].position[0], -3.0f);
    EXPECT_FLOAT_EQ(artifact->vertices[1].position[1], 5.0f);
    EXPECT_FLOAT_EQ(artifact->vertices[2].position[2], 9.0f);
    EXPECT_TRUE(artifact->hasBoundingSphere);

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_TRUE(std::none_of(
        telemetry.begin(),
        telemetry.end(),
        [](const NLS::Core::Assets::ArtifactLoadTelemetryRecord& record)
        {
            return record.stage ==
                NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeContainerParseHash;
        }));

    std::filesystem::remove_all(root);
}

TEST(MeshBoundingSphereTests, BuildsVertexUploadViewDirectlyFromStructuredVertices)
{
    const std::vector<NLS::Render::Geometry::Vertex> vertices{
        VertexAt(1.0f, 2.0f, 3.0f),
        VertexAt(4.0f, 5.0f, 6.0f)
    };

    const auto uploadView = NLS::Render::Resources::BuildMeshVertexUploadView(vertices);

    EXPECT_EQ(uploadView.data, vertices.data());
    EXPECT_EQ(uploadView.byteSize, vertices.size() * sizeof(NLS::Render::Geometry::Vertex));
    EXPECT_EQ(uploadView.stride, sizeof(NLS::Render::Geometry::Vertex));
    EXPECT_EQ(uploadView.positionOffset, 0u);
    EXPECT_EQ(uploadView.texCoordOffset, sizeof(float) * 3u);
    EXPECT_EQ(uploadView.normalOffset, sizeof(float) * 5u);
    EXPECT_EQ(uploadView.tangentOffset, sizeof(float) * 8u);
    EXPECT_EQ(uploadView.bitangentOffset, sizeof(float) * 11u);
}

TEST(MeshResourceTests, ReusesRhiMeshAdapterForStableMeshDraws)
{
    EnsureMeshTestDriver();

    const std::vector<NLS::Render::Geometry::Vertex> vertices{
        VertexAt(0.0f, 0.0f, 0.0f),
        VertexAt(1.0f, 0.0f, 0.0f),
        VertexAt(0.0f, 1.0f, 0.0f)
    };
    const std::vector<uint32_t> indices{0u, 1u, 2u};
    NLS::Render::Resources::Mesh mesh(vertices, indices, 0u);

    const auto first = mesh.GetRHIMesh();
    const auto second = mesh.GetRHIMesh();

    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first, second);
}

TEST(MeshResourceTests, MeshIsNotMovableAfterCachingSelfReferentialRhiAdapter)
{
    using NLS::Render::Resources::Mesh;

    EXPECT_FALSE(std::is_copy_constructible_v<Mesh>);
    EXPECT_FALSE(std::is_copy_assignable_v<Mesh>);
    EXPECT_FALSE(std::is_move_constructible_v<Mesh>);
    EXPECT_FALSE(std::is_move_assignable_v<Mesh>);
}

TEST(MeshResourceTests, MeshCanReusePrecomputedArtifactBoundingSphere)
{
    EnsureMeshTestDriver();

    const std::vector<NLS::Render::Geometry::Vertex> vertices{
        VertexAt(-1000.0f, -1000.0f, -1000.0f),
        VertexAt(1000.0f, 1000.0f, 1000.0f),
        VertexAt(0.0f, 0.0f, 0.0f)
    };
    const std::vector<uint32_t> indices{0u, 1u, 2u};
    NLS::Render::Geometry::BoundingSphere artifactBounds;
    artifactBounds.position = { 1.0f, 2.0f, 3.0f };
    artifactBounds.radius = 4.0f;

    const NLS::Render::Resources::Mesh mesh(
        vertices,
        indices,
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
        artifactBounds);

    const auto& sphere = mesh.GetBoundingSphere();
    EXPECT_FLOAT_EQ(sphere.position.x, 1.0f);
    EXPECT_FLOAT_EQ(sphere.position.y, 2.0f);
    EXPECT_FLOAT_EQ(sphere.position.z, 3.0f);
    EXPECT_FLOAT_EQ(sphere.radius, 4.0f);

    const auto& bounds = mesh.GetBounds();
    EXPECT_FLOAT_EQ(bounds.center.x, 0.0f);
    EXPECT_FLOAT_EQ(bounds.center.y, 0.0f);
    EXPECT_FLOAT_EQ(bounds.center.z, 0.0f);
    EXPECT_FLOAT_EQ(bounds.size.x, 2000.0f);
    EXPECT_FLOAT_EQ(bounds.size.y, 2000.0f);
    EXPECT_FLOAT_EQ(bounds.size.z, 2000.0f);
}

TEST(MeshBoundsTests, PartialVertexUpdateBoundsUseAABBUnionInsteadOfBoundingSphereCube)
{
    const std::vector<NLS::Render::Geometry::Vertex> vertices{
        VertexAt(0.0f, 0.0f, 0.0f),
        VertexAt(1.0f, 1.0f, 1.0f),
        VertexAt(2.0f, 2.0f, 2.0f)
    };
    const auto bounds = NLS::Render::Geometry::UnionBounds(
        NLS::Render::Geometry::ComputeBounds(vertices),
        NLS::Render::Geometry::ComputeBounds({ VertexAt(3.0f, 4.0f, 5.0f) }));

    EXPECT_FLOAT_EQ(bounds.center.x, 1.5f);
    EXPECT_FLOAT_EQ(bounds.center.y, 2.0f);
    EXPECT_FLOAT_EQ(bounds.center.z, 2.5f);
    EXPECT_FLOAT_EQ(bounds.size.x, 3.0f);
    EXPECT_FLOAT_EQ(bounds.size.y, 4.0f);
    EXPECT_FLOAT_EQ(bounds.size.z, 5.0f);
}

TEST(MeshArtifactTests, SerializationPersistsPrecomputedBoundingSphere)
{
    const std::vector<NLS::Render::Geometry::Vertex> vertices{
        VertexAt(-1.0f, -2.0f, -3.0f),
        VertexAt(5.0f, 6.0f, 7.0f),
        VertexAt(2.0f, 3.0f, 4.0f)
    };
    const std::vector<uint32_t> indices{0u, 1u, 2u};
    const auto bytes = NLS::Render::Assets::SerializeMeshArtifact({vertices, indices, 5u});
    const auto artifact = NLS::Render::Assets::DeserializeMeshArtifact(bytes);

    ASSERT_TRUE(artifact.has_value());
    EXPECT_TRUE(artifact->hasBoundingSphere);
    EXPECT_FLOAT_EQ(artifact->boundingSphere.position.x, 2.0f);
    EXPECT_FLOAT_EQ(artifact->boundingSphere.position.y, 2.0f);
    EXPECT_FLOAT_EQ(artifact->boundingSphere.position.z, 2.0f);
    EXPECT_GT(artifact->boundingSphere.radius, 0.0f);
}

TEST(MeshArtifactTests, SerializationPreservesExplicitZeroRadiusBoundingSphere)
{
    const std::vector<NLS::Render::Geometry::Vertex> vertices{
        VertexAt(-10.0f, 0.0f, 0.0f),
        VertexAt(10.0f, 0.0f, 0.0f)
    };
    const std::vector<uint32_t> indices{0u, 1u};

    NLS::Render::Assets::MeshArtifactData mesh;
    mesh.vertices = vertices;
    mesh.indices = indices;
    mesh.materialIndex = 2u;
    mesh.boundingSphere.position = { 7.0f, 8.0f, 9.0f };
    mesh.boundingSphere.radius = 0.0f;
    mesh.hasBoundingSphere = true;

    const auto bytes = NLS::Render::Assets::SerializeMeshArtifact(mesh);
    const auto artifact = NLS::Render::Assets::DeserializeMeshArtifact(bytes);

    ASSERT_TRUE(artifact.has_value());
    EXPECT_TRUE(artifact->hasBoundingSphere);
    EXPECT_FLOAT_EQ(artifact->boundingSphere.position.x, 7.0f);
    EXPECT_FLOAT_EQ(artifact->boundingSphere.position.y, 8.0f);
    EXPECT_FLOAT_EQ(artifact->boundingSphere.position.z, 9.0f);
    EXPECT_FLOAT_EQ(artifact->boundingSphere.radius, 0.0f);
}

TEST(MeshArtifactTests, VersionTwoHeaderAndBoundsUseStableFieldOrder)
{
    NLS::Render::Assets::MeshArtifactData mesh;
    mesh.materialIndex = 0x01020304u;
    mesh.boundingSphere.position = { 1.0f, 2.0f, 3.0f };
    mesh.boundingSphere.radius = 4.0f;
    mesh.hasBoundingSphere = true;

    const auto bytes = NLS::Render::Assets::SerializeMeshArtifact(mesh);

    const std::vector<uint8_t> expectedPrefix{
        0x4Eu, 0x4Eu, 0x4Du, 0x48u,
        0x02u, 0x00u, 0x00u, 0x00u,
        static_cast<uint8_t>(sizeof(NLS::Render::Geometry::Vertex) & 0xFFu),
        static_cast<uint8_t>((sizeof(NLS::Render::Geometry::Vertex) >> 8u) & 0xFFu),
        0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u,
        0x04u, 0x03u, 0x02u, 0x01u,
        0x00u, 0x00u, 0x80u, 0x3Fu,
        0x00u, 0x00u, 0x00u, 0x40u,
        0x00u, 0x00u, 0x40u, 0x40u,
        0x00u, 0x00u, 0x80u, 0x40u
    };

    const auto payload = ReadMeshArtifactPayload(bytes);
    ASSERT_GE(payload.size(), expectedPrefix.size());
    EXPECT_TRUE(std::equal(expectedPrefix.begin(), expectedPrefix.end(), payload.begin()));
}

TEST(MeshArtifactTests, DeserializesLegacyV1ArtifactByComputingBoundingSphere)
{
    const std::vector<NLS::Render::Geometry::Vertex> vertices{
        VertexAt(-2.0f, -4.0f, -6.0f),
        VertexAt(6.0f, 8.0f, 10.0f)
    };
    const std::vector<uint32_t> indices{0u, 1u};
    const auto bytes = WrapMeshArtifactPayload(BuildLegacyV1MeshArtifactBytes(vertices, indices, 9u));

    const auto artifact = NLS::Render::Assets::DeserializeMeshArtifact(bytes);

    ASSERT_TRUE(artifact.has_value());
    EXPECT_TRUE(artifact->hasBoundingSphere);
    EXPECT_EQ(artifact->materialIndex, 9u);
    EXPECT_FLOAT_EQ(artifact->boundingSphere.position.x, 2.0f);
    EXPECT_FLOAT_EQ(artifact->boundingSphere.position.y, 2.0f);
    EXPECT_FLOAT_EQ(artifact->boundingSphere.position.z, 2.0f);
    EXPECT_GT(artifact->boundingSphere.radius, 0.0f);
}

TEST(MeshArtifactTests, RejectsTruncatedVersionTwoBoundingSpherePayload)
{
    const std::vector<NLS::Render::Geometry::Vertex> vertices{
        VertexAt(0.0f, 0.0f, 0.0f),
        VertexAt(1.0f, 0.0f, 0.0f)
    };
    const std::vector<uint32_t> indices{0u, 1u};
    const auto bytes = NLS::Render::Assets::SerializeMeshArtifact({vertices, indices, 1u});

    ASSERT_GT(bytes.size(), 0u);
    auto truncated = bytes;
    truncated.pop_back();

    EXPECT_FALSE(NLS::Render::Assets::DeserializeMeshArtifact(truncated).has_value());
}

TEST(MeshManagerTests, ResolvesProjectLibraryMeshArtifactsFromProjectRoot)
{
    EnsureMeshTestDriver();

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_project_library_model_artifact_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Project" / "Assets";
    const auto engineAssets = root / "EngineAssets";
    const auto artifactPath = root / "Project" / "Library" / "Artifacts" / "Hero" / "db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb";

    std::filesystem::create_directories(projectAssets);
    std::filesystem::create_directories(engineAssets);
    std::filesystem::create_directories(artifactPath.parent_path());

    const std::vector<NLS::Render::Geometry::Vertex> vertices{
        VertexAt(0.0f, 0.0f, 0.0f),
        VertexAt(4.0f, 0.0f, 0.0f),
        VertexAt(0.0f, 4.0f, 0.0f)
    };
    const std::vector<uint32_t> indices{0u, 1u, 2u};
    const auto bytes = NLS::Render::Assets::SerializeMeshArtifact({vertices, indices, 0u});
    {
        std::ofstream artifact(artifactPath, std::ios::binary | std::ios::trunc);
        artifact.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
        WithTrailingSlash(projectAssets),
        WithTrailingSlash(engineAssets));
    NLS::Core::ResourceManagement::MeshManager manager;

    auto* mesh = manager["Library/Artifacts/Hero/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb"];
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->GetVertexCount(), 3u);

    std::filesystem::remove_all(root);
}

TEST(MeshManagerTests, PrimitiveAliasLoadsBuiltinMeshArtifactWithoutExposingModelPath)
{
    EnsureMeshTestDriver();

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_primitive_alias_mesh_artifact_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Project" / "Assets";
    const auto engineAssets = root / "EngineAssets";
    const auto sourceModelPath = engineAssets / "Models" / "Cube.fbx";
    const auto artifactPath = BuiltinMeshArtifactPath(engineAssets, "Models/Cube.fbx");

    std::filesystem::create_directories(sourceModelPath.parent_path());
    std::filesystem::create_directories(artifactPath.parent_path());
    {
        std::ofstream invalidSource(sourceModelPath, std::ios::binary | std::ios::trunc);
        invalidSource << "not parsed when preimported primitive artifact exists";
    }

    const std::vector<NLS::Render::Geometry::Vertex> vertices{
        VertexAt(0.0f, 0.0f, 0.0f),
        VertexAt(3.0f, 0.0f, 0.0f),
        VertexAt(0.0f, 3.0f, 0.0f)
    };
    const std::vector<uint32_t> indices{0u, 1u, 2u};
    const auto bytes = NLS::Render::Assets::SerializeMeshArtifact({vertices, indices, 7u});
    {
        std::ofstream artifact(artifactPath, std::ios::binary | std::ios::trunc);
        artifact.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
        WithTrailingSlash(projectAssets),
        WithTrailingSlash(engineAssets));
    NLS::Core::ResourceManagement::MeshManager manager;

    auto* mesh = manager["builtin:Primitive/Cube"];

    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->GetVertexCount(), 3u);
    EXPECT_EQ(mesh->GetIndexCount(), 3u);
    EXPECT_EQ(mesh->GetMaterialIndex(), 7u);
    EXPECT_TRUE(manager.IsResourceRegistered("builtin:Primitive/Cube"));
    EXPECT_FALSE(manager.IsResourceRegistered(":Models\\Cube.fbx"));

    manager.UnloadResources();
    std::filesystem::remove_all(root);
}

TEST(MeshManagerTests, MeshArtifactLoadsDoNotReadLegacyModelPackages)
{
    EnsureMeshTestDriver();

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_project_library_mesh_without_model_package_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Project" / "Assets";
    const auto engineAssets = root / "EngineAssets";
    const auto artifactRoot = root / "Project" / "Library" / "Artifacts" / "Hero";
    const auto bodyPath = artifactRoot / "meshes" / "db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb";
    const auto roofPath = artifactRoot / "meshes" / "ae885f485f82848c6eb30f0fd29961c7726f140d870c77a4c2a378b86ea38e07";
    const auto staleModelBlobPath =
        artifactRoot / "5d4b4d6c2b6c4a6c9b91d90753df2a8d5d4b4d6c2b6c4a6c9b91d90753df2a8d";

    std::filesystem::create_directories(projectAssets);
    std::filesystem::create_directories(engineAssets);
    std::filesystem::create_directories(bodyPath.parent_path());

    const std::vector<NLS::Render::Geometry::Vertex> bodyVertices{
        VertexAt(0.0f, 0.0f, 0.0f),
        VertexAt(1.0f, 0.0f, 0.0f),
        VertexAt(0.0f, 1.0f, 0.0f)
    };
    const std::vector<NLS::Render::Geometry::Vertex> roofVertices{
        VertexAt(0.0f, 0.0f, 1.0f),
        VertexAt(2.0f, 0.0f, 1.0f),
        VertexAt(0.0f, 2.0f, 1.0f)
    };
    const std::vector<uint32_t> indices{0u, 1u, 2u};
    {
        const auto bytes = NLS::Render::Assets::SerializeMeshArtifact({bodyVertices, indices, 0u});
        std::ofstream artifact(bodyPath, std::ios::binary | std::ios::trunc);
        artifact.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    {
        const auto bytes = NLS::Render::Assets::SerializeMeshArtifact({roofVertices, indices, 1u});
        std::ofstream artifact(roofPath, std::ios::binary | std::ios::trunc);
        artifact.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    {
        std::ofstream package(staleModelBlobPath, std::ios::binary | std::ios::trunc);
        package << "stale model package blob must not be read";
    }

    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
        WithTrailingSlash(projectAssets),
        WithTrailingSlash(engineAssets));
    NLS::Core::ResourceManagement::MeshManager manager;

    auto* body = manager["Library/Artifacts/Hero/meshes/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb"];

    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->GetVertexCount(), 3u);
    EXPECT_FALSE(manager.IsResourceRegistered(
        "Library/Artifacts/Hero/5d4b4d6c2b6c4a6c9b91d90753df2a8d5d4b4d6c2b6c4a6c9b91d90753df2a8d"));
    EXPECT_FALSE(manager.IsResourceRegistered("Library/Artifacts/Hero/meshes/ae885f485f82848c6eb30f0fd29961c7726f140d870c77a4c2a378b86ea38e07"));

    std::filesystem::remove_all(root);
}

TEST(MeshManagerTests, DirectInvalidModelPackageBlobLoadsAreRejected)
{
    EnsureMeshTestDriver();

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_direct_legacy_model_package_rejected_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Project" / "Assets";
    const auto engineAssets = root / "EngineAssets";
    const auto artifactRoot = root / "Project" / "Library" / "Artifacts" / "Hero";
    const auto packagePath =
        artifactRoot / "5d4b4d6c2b6c4a6c9b91d90753df2a8d5d4b4d6c2b6c4a6c9b91d90753df2a8d";

    std::filesystem::create_directories(projectAssets);
    std::filesystem::create_directories(engineAssets);
    std::filesystem::create_directories(packagePath.parent_path());
    {
        std::ofstream package(packagePath, std::ios::binary | std::ios::trunc);
        package << "invalid model package blob must not be read";
    }

    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
        WithTrailingSlash(projectAssets),
        WithTrailingSlash(engineAssets));
    NLS::Core::ResourceManagement::MeshManager manager;

    auto* package = manager[
        "Library/Artifacts/Hero/5d4b4d6c2b6c4a6c9b91d90753df2a8d5d4b4d6c2b6c4a6c9b91d90753df2a8d"];

    EXPECT_EQ(package, nullptr);
    EXPECT_FALSE(manager.IsResourceRegistered(
        "Library/Artifacts/Hero/5d4b4d6c2b6c4a6c9b91d90753df2a8d5d4b4d6c2b6c4a6c9b91d90753df2a8d"));

    std::filesystem::remove_all(root);
}

TEST(MeshManagerTests, ReloadResourceUpdatesLoadedMeshFromArtifact)
{
    EnsureMeshTestDriver();

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_mesh_reload_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Project" / "Assets";
    const auto engineAssets = root / "EngineAssets";
    const auto artifactPath = root / "Project" / "Library" / "Artifacts" / "Hero" / "db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb";
    std::filesystem::create_directories(artifactPath.parent_path());

    WriteMeshArtifact(
        artifactPath,
        { VertexAt(0.0f, 0.0f, 0.0f), VertexAt(1.0f, 0.0f, 0.0f), VertexAt(0.0f, 1.0f, 0.0f) },
        {0u, 1u, 2u},
        2u);

    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
        WithTrailingSlash(projectAssets),
        WithTrailingSlash(engineAssets));
    NLS::Core::ResourceManagement::MeshManager manager;

    auto* mesh = manager["Library/Artifacts/Hero/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb"];
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->GetVertexCount(), 3u);
    EXPECT_EQ(mesh->GetMaterialIndex(), 2u);

    WriteMeshArtifact(
        artifactPath,
        {
            VertexAt(0.0f, 0.0f, 0.0f),
            VertexAt(1.0f, 0.0f, 0.0f),
            VertexAt(1.0f, 1.0f, 0.0f),
            VertexAt(0.0f, 1.0f, 0.0f)
        },
        {0u, 1u, 2u, 0u, 2u, 3u},
        7u);

    manager.AResourceManager::ReloadResource("Library/Artifacts/Hero/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb");

    ASSERT_EQ(manager.GetResource("Library/Artifacts/Hero/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb", false), mesh);
    EXPECT_EQ(mesh->GetVertexCount(), 4u);
    EXPECT_EQ(mesh->GetIndexCount(), 6u);
    EXPECT_EQ(mesh->GetMaterialIndex(), 7u);

    manager.UnloadResources();
    std::filesystem::remove_all(root);
}

TEST(MeshManagerTests, BuiltinSourcePathPrefersPreimportedMeshArtifact)
{
    EnsureMeshTestDriver();

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_builtin_model_artifact_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "ProjectAssets";
    const auto engineAssets = root / "EngineAssets";
    const auto sourceModelPath = engineAssets / "Models" / "Cube.fbx";
    const auto artifactPath = BuiltinMeshArtifactPath(engineAssets, "Models/Cube.fbx");

    std::filesystem::create_directories(sourceModelPath.parent_path());
    std::filesystem::create_directories(artifactPath.parent_path());
    {
        std::ofstream invalidSource(sourceModelPath, std::ios::binary | std::ios::trunc);
        invalidSource << "not a parseable fbx source";
    }

    const std::vector<NLS::Render::Geometry::Vertex> vertices{
        VertexAt(0.0f, 0.0f, 0.0f),
        VertexAt(3.0f, 0.0f, 0.0f),
        VertexAt(0.0f, 3.0f, 0.0f)
    };
    const std::vector<uint32_t> indices{0u, 1u, 2u};
    const auto bytes = NLS::Render::Assets::SerializeMeshArtifact({vertices, indices, 7u});
    {
        std::ofstream artifact(artifactPath, std::ios::binary | std::ios::trunc);
        artifact.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
        WithTrailingSlash(projectAssets),
        WithTrailingSlash(engineAssets));
    NLS::Core::ResourceManagement::MeshManager manager;

    auto* mesh = manager[":Models\\Cube.fbx"];

    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->GetVertexCount(), 3u);
    EXPECT_EQ(mesh->GetIndexCount(), 3u);
    EXPECT_EQ(mesh->GetMaterialIndex(), 7u);

    manager.UnloadResources();
    std::filesystem::remove_all(root);
}

TEST(MeshManagerTests, BuiltinSourcePathPrefersProjectLibraryCacheBeforeBundledArtifact)
{
    EnsureMeshTestDriver();

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_builtin_model_project_cache_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Project" / "Assets";
    const auto engineAssets = root / "EngineAssets";
    const auto sourceModelPath = engineAssets / "Models" / "Cube.fbx";
    const auto projectArtifactPath = BuiltinMeshArtifactPath(root / "Project", "Models/Cube.fbx");
    const auto bundledArtifactPath = BuiltinMeshArtifactPath(engineAssets, "Models/Cube.fbx");

    std::filesystem::create_directories(sourceModelPath.parent_path());
    std::filesystem::create_directories(projectArtifactPath.parent_path());
    std::filesystem::create_directories(bundledArtifactPath.parent_path());
    {
        std::ofstream invalidSource(sourceModelPath, std::ios::binary | std::ios::trunc);
        invalidSource << "not a parseable fbx source";
    }

    const auto writeArtifact = [](const std::filesystem::path& path, uint32_t materialIndex)
    {
        const std::vector<NLS::Render::Geometry::Vertex> vertices{
            VertexAt(0.0f, 0.0f, 0.0f),
            VertexAt(1.0f, 0.0f, 0.0f),
            VertexAt(0.0f, 1.0f, 0.0f)
        };
        const std::vector<uint32_t> indices{0u, 1u, 2u};
        const auto bytes = NLS::Render::Assets::SerializeMeshArtifact({vertices, indices, materialIndex});
        std::ofstream artifact(path, std::ios::binary | std::ios::trunc);
        artifact.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };
    writeArtifact(projectArtifactPath, 11u);
    writeArtifact(bundledArtifactPath, 22u);

    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
        WithTrailingSlash(projectAssets),
        WithTrailingSlash(engineAssets));
    NLS::Core::ResourceManagement::MeshManager manager;

    auto* mesh = manager[":Models/Cube.fbx"];

    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->GetMaterialIndex(), 11u);

    manager.UnloadResources();
    std::filesystem::remove_all(root);
}

TEST(MeshManagerTests, BuiltinSourcePathLazilyWritesProjectLibraryArtifactWhenCacheIsMissing)
{
    EnsureMeshTestDriver();

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_builtin_model_lazy_cache_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Project" / "Assets";
    const auto engineAssets = root / "EngineAssets";
    const auto sourceModelPath = engineAssets / "Models" / "Triangle.obj";
    const auto projectArtifactPath = BuiltinMeshArtifactPath(root / "Project", "Models/Triangle.obj");

    std::filesystem::create_directories(sourceModelPath.parent_path());
    {
        std::ofstream source(sourceModelPath, std::ios::trunc);
        source <<
            "v 0 0 0\n"
            "v 2 0 0\n"
            "v 0 2 0\n"
            "vn 0 0 1\n"
            "vt 0 0\n"
            "vt 1 0\n"
            "vt 0 1\n"
            "f 1/1/1 2/2/1 3/3/1\n";
    }

    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
        WithTrailingSlash(projectAssets),
        WithTrailingSlash(engineAssets));
    NLS::Core::ResourceManagement::MeshManager manager;

    auto* mesh = manager[":Models/Triangle.obj"];

    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->GetVertexCount(), 3u);
    EXPECT_TRUE(std::filesystem::is_regular_file(projectArtifactPath));

    manager.UnloadResources();
    std::filesystem::remove_all(root);
}

TEST(MeshManagerTests, BuiltinSourcePathUsesBundledArtifactBeforeLazyGeneration)
{
    EnsureMeshTestDriver();

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_builtin_model_bundled_first_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Project" / "Assets";
    const auto engineAssets = root / "EngineAssets";
    const auto sourceModelPath = engineAssets / "Models" / "Triangle.obj";
    const auto projectArtifactPath = BuiltinMeshArtifactPath(root / "Project", "Models/Triangle.obj");
    const auto bundledArtifactPath = BuiltinMeshArtifactPath(engineAssets, "Models/Triangle.obj");

    std::filesystem::create_directories(sourceModelPath.parent_path());
    std::filesystem::create_directories(bundledArtifactPath.parent_path());
    {
        std::ofstream source(sourceModelPath, std::ios::trunc);
        source <<
            "v 0 0 0\n"
            "v 2 0 0\n"
            "v 0 2 0\n"
            "f 1 2 3\n";
    }

    const std::vector<NLS::Render::Geometry::Vertex> vertices{
        VertexAt(0.0f, 0.0f, 0.0f),
        VertexAt(1.0f, 0.0f, 0.0f),
        VertexAt(0.0f, 1.0f, 0.0f)
    };
    const std::vector<uint32_t> indices{0u, 1u, 2u};
    const auto bytes = NLS::Render::Assets::SerializeMeshArtifact({vertices, indices, 33u});
    {
        std::ofstream bundled(bundledArtifactPath, std::ios::binary | std::ios::trunc);
        bundled.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
        WithTrailingSlash(projectAssets),
        WithTrailingSlash(engineAssets));
    NLS::Core::ResourceManagement::MeshManager manager;

    auto* mesh = manager[":Models/Triangle.obj"];

    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->GetMaterialIndex(), 33u);
    EXPECT_FALSE(std::filesystem::exists(projectArtifactPath));

    manager.UnloadResources();
    std::filesystem::remove_all(root);
}
