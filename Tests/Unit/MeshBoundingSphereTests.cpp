#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <atomic>
#include <type_traits>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Guid.h"
#include "Assets/NativeArtifactContainer.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Context/Driver.h"
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

TEST(MeshBoundingSphereTests, LoadMeshArtifactHonorsCancellationBeforeReading)
{
    const auto root = std::filesystem::temp_directory_path() / ("nullus_mesh_cancel_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    const auto artifactPath = root / "cancelled.nmesh";

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
    const auto artifactPath = root / "Project" / "Library" / "Artifacts" / "Hero" / "body.nmesh";

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

    auto* mesh = manager["Library/Artifacts/Hero/body.nmesh"];
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
    const auto artifactPath = engineAssets / "Library" / "BuiltinArtifacts" / "Models" / "Cube.nmesh";

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
    const auto bodyPath = artifactRoot / "meshes" / "body.nmesh";
    const auto roofPath = artifactRoot / "meshes" / "roof.nmesh";
    const auto packagePath = artifactRoot / "model.nmodel";

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
        std::ofstream package(packagePath, std::ios::binary | std::ios::trunc);
        package << "legacy nmodel package must not be read";
    }

    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
        WithTrailingSlash(projectAssets),
        WithTrailingSlash(engineAssets));
    NLS::Core::ResourceManagement::MeshManager manager;

    auto* body = manager["Library/Artifacts/Hero/meshes/body.nmesh"];

    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->GetVertexCount(), 3u);
    EXPECT_FALSE(manager.IsResourceRegistered("Library/Artifacts/Hero/model.nmodel"));
    EXPECT_FALSE(manager.IsResourceRegistered("Library/Artifacts/Hero/meshes/roof.nmesh"));

    std::filesystem::remove_all(root);
}

TEST(MeshManagerTests, DirectLegacyModelPackageLoadsAreRejected)
{
    EnsureMeshTestDriver();

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_direct_legacy_model_package_rejected_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Project" / "Assets";
    const auto engineAssets = root / "EngineAssets";
    const auto artifactRoot = root / "Project" / "Library" / "Artifacts" / "Hero";
    const auto packagePath = artifactRoot / "model.nmodel";

    std::filesystem::create_directories(projectAssets);
    std::filesystem::create_directories(engineAssets);
    std::filesystem::create_directories(packagePath.parent_path());
    {
        std::ofstream package(packagePath, std::ios::binary | std::ios::trunc);
        package << "legacy nmodel package must not be read";
    }

    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
        WithTrailingSlash(projectAssets),
        WithTrailingSlash(engineAssets));
    NLS::Core::ResourceManagement::MeshManager manager;

    auto* package = manager["Library/Artifacts/Hero/model.nmodel"];

    EXPECT_EQ(package, nullptr);
    EXPECT_FALSE(manager.IsResourceRegistered("Library/Artifacts/Hero/model.nmodel"));

    std::filesystem::remove_all(root);
}

TEST(MeshManagerTests, ReloadResourceUpdatesLoadedMeshFromArtifact)
{
    EnsureMeshTestDriver();

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_mesh_reload_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Project" / "Assets";
    const auto engineAssets = root / "EngineAssets";
    const auto artifactPath = root / "Project" / "Library" / "Artifacts" / "Hero" / "body.nmesh";
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

    auto* mesh = manager["Library/Artifacts/Hero/body.nmesh"];
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

    manager.AResourceManager::ReloadResource("Library/Artifacts/Hero/body.nmesh");

    ASSERT_EQ(manager.GetResource("Library/Artifacts/Hero/body.nmesh", false), mesh);
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
    const auto artifactPath = engineAssets / "Library" / "BuiltinArtifacts" / "Models" / "Cube.nmesh";

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
    const auto projectArtifactPath = root / "Project" / "Library" / "BuiltinArtifacts" / "Models" / "Cube.nmesh";
    const auto bundledArtifactPath = engineAssets / "Library" / "BuiltinArtifacts" / "Models" / "Cube.nmesh";

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
    const auto projectArtifactPath = root / "Project" / "Library" / "BuiltinArtifacts" / "Models" / "Triangle.nmesh";

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
    const auto projectArtifactPath = root / "Project" / "Library" / "BuiltinArtifacts" / "Models" / "Triangle.nmesh";
    const auto bundledArtifactPath = engineAssets / "Library" / "BuiltinArtifacts" / "Models" / "Triangle.nmesh";

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
