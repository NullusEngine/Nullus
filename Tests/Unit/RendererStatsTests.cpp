#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Core/RendererStats.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/DriverSettings.h"

namespace
{
    class StatsOnlyRenderer final : public NLS::Render::Core::CompositeRenderer
    {
    public:
        explicit StatsOnlyRenderer(NLS::Render::Context::Driver& driver)
            : CompositeRenderer(driver)
        {
        }

    protected:
        bool PrepareRecordedDraw(
            PipelineState,
            const NLS::Render::Entities::Drawable& drawable,
            PreparedRecordedDraw& outDraw) const override
        {
            if (drawable.material == nullptr)
                return false;

            outDraw.instanceCount = static_cast<uint32_t>(std::max(drawable.material->GetGPUInstances(), 0));
            return outDraw.instanceCount > 0u;
        }

        void BindPreparedGraphicsPipeline(const PreparedRecordedDraw&) const override {}
        void BindPreparedMaterialBindingSet(const PreparedRecordedDraw&) const override {}
        void SubmitPreparedDraw(const PreparedRecordedDraw&) const override {}
    };

    std::unique_ptr<NLS::Render::Resources::Mesh> CreateTriangleMesh()
    {
        std::vector<NLS::Render::Geometry::Vertex> vertices(3);
        vertices[0].position[0] = 0.0f;
        vertices[1].position[0] = 1.0f;
        vertices[2].position[1] = 1.0f;
        std::vector<uint32_t> indices { 0u, 1u, 2u };
        return std::make_unique<NLS::Render::Resources::Mesh>(vertices, indices, 0u);
    }
}

TEST(RendererStatsTests, RendererStatsTracksSubmittedDrawCounts)
{
    NLS::Render::Core::RendererStats stats;
    NLS::Render::Resources::Material material;
    material.SetGPUInstances(2);
    const auto mesh = CreateTriangleMesh();

    NLS::Render::Entities::Drawable drawable;
    drawable.mesh = mesh.get();
    drawable.material = &material;

    stats.BeginFrame();
    stats.RecordSubmittedDraw(drawable, static_cast<uint32_t>(material.GetGPUInstances()));
    stats.EndFrame();

    const auto& frameInfo = stats.GetFrameInfo();
    EXPECT_EQ(frameInfo.batchCount, 1u);
    EXPECT_EQ(frameInfo.instanceCount, 2u);
    EXPECT_EQ(frameInfo.polyCount, 2u);
    EXPECT_EQ(frameInfo.vertexCount, 6u);
}

TEST(RendererStatsTests, CompositeRendererExposesFinalizedFrameInfoWithoutFeatureRegistration)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    StatsOnlyRenderer renderer(*driver);
    NLS::Render::Resources::Material material;
    material.SetGPUInstances(3);
    const auto mesh = CreateTriangleMesh();

    NLS::Render::Entities::Drawable drawable;
    drawable.mesh = mesh.get();
    drawable.material = &material;

    renderer.ResetFrameStatistics();
    renderer.DrawEntity(NLS::Render::Data::PipelineState {}, drawable);
    renderer.FinalizeFrameStatistics();

    const auto& frameInfo = renderer.GetFrameInfo();
    EXPECT_EQ(frameInfo.batchCount, 1u);
    EXPECT_EQ(frameInfo.instanceCount, 3u);
    EXPECT_EQ(frameInfo.polyCount, 3u);
    EXPECT_EQ(frameInfo.vertexCount, 9u);
}

TEST(RendererStatsTests, CompositeRendererFinalizedFrameInfoIsZeroWhenNoDrawsWereSubmitted)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    StatsOnlyRenderer renderer(*driver);
    renderer.ResetFrameStatistics();
    renderer.FinalizeFrameStatistics();

    const auto& frameInfo = renderer.GetFrameInfo();
    EXPECT_EQ(frameInfo.batchCount, 0u);
    EXPECT_EQ(frameInfo.instanceCount, 0u);
    EXPECT_EQ(frameInfo.polyCount, 0u);
    EXPECT_EQ(frameInfo.vertexCount, 0u);
}
