#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/Resources/Loaders/ModelLoader.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"

namespace
{
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

TEST(ModelBoundingSphereTests, CreateFromMeshesComputesBoundingSphere)
{
    EnsureMeshTestDriver();

    auto* mesh = new NLS::Render::Resources::Mesh(
        {
            VertexAt(2.0f, 4.0f, 6.0f),
            VertexAt(6.0f, 8.0f, 10.0f),
            VertexAt(4.0f, 6.0f, 8.0f)
        },
        { 0u, 1u, 2u },
        0u);

    auto* model = NLS::Render::Resources::Loaders::ModelLoader::Create({ mesh });
    ASSERT_NE(model, nullptr);

    const auto& sphere = model->GetBoundingSphere();
    EXPECT_FLOAT_EQ(sphere.position.x, 4.0f);
    EXPECT_FLOAT_EQ(sphere.position.y, 6.0f);
    EXPECT_FLOAT_EQ(sphere.position.z, 8.0f);
    EXPECT_GT(sphere.radius, 0.0f);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ModelLoader::Destroy(model));
}

TEST(ModelBoundingSphereTests, ComputesCenterFromAllNegativeMeshBounds)
{
    EnsureMeshTestDriver();

    auto* meshA = new NLS::Render::Resources::Mesh(
        {
            VertexAt(-8.0f, -6.0f, -4.0f),
            VertexAt(-2.0f, -10.0f, -12.0f),
            VertexAt(-5.0f, -3.0f, -7.0f)
        },
        { 0u, 1u, 2u },
        0u);
    auto* meshB = new NLS::Render::Resources::Mesh(
        {
            VertexAt(-18.0f, -16.0f, -14.0f),
            VertexAt(-12.0f, -20.0f, -22.0f),
            VertexAt(-15.0f, -13.0f, -17.0f)
        },
        { 0u, 1u, 2u },
        0u);

    auto* model = NLS::Render::Resources::Loaders::ModelLoader::Create({ meshA, meshB });
    ASSERT_NE(model, nullptr);

    const auto& sphere = model->GetBoundingSphere();
    EXPECT_FLOAT_EQ(sphere.position.x, -10.0f);
    EXPECT_FLOAT_EQ(sphere.position.y, -11.5f);
    EXPECT_FLOAT_EQ(sphere.position.z, -13.0f);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ModelLoader::Destroy(model));
}
