#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Debug/DebugDrawPass.h"
#include "Rendering/Debug/DebugDrawService.h"
#include "Rendering/Settings/DriverSettings.h"

namespace
{
    class PassRecordingRenderer final : public NLS::Render::Core::CompositeRenderer
    {
    public:
        explicit PassRecordingRenderer(NLS::Render::Context::Driver& driver)
            : CompositeRenderer(driver)
        {
        }
    };

    class RecordingDebugDrawPass final : public NLS::Render::Debug::DebugDrawPass
    {
    public:
        explicit RecordingDebugDrawPass(NLS::Render::Core::CompositeRenderer& renderer)
            : DebugDrawPass(renderer)
        {
        }

        void ExecuteForTest(NLS::Render::Data::PipelineState pipelineState)
        {
            Draw(pipelineState);
        }

        std::vector<NLS::Render::Debug::DebugDrawPrimitive> recordedPrimitives;

    protected:
        void RenderPrimitive(const NLS::Render::Debug::DebugDrawPrimitive& primitive, NLS::Render::Data::PipelineState) override
        {
            recordedPrimitives.push_back(primitive);
        }
    };
}

TEST(DebugDrawPassTests, ExplicitPassConsumesOnlyVisiblePrimitivesOncePerExecution)
{
    using namespace NLS::Render::Debug;

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    PassRecordingRenderer renderer(*driver);
    renderer.SetDebugDrawService(std::make_unique<DebugDrawService>(8u));

    auto* service = renderer.GetDebugDrawService();
    ASSERT_NE(service, nullptr);

    DebugDrawSubmitOptions hiddenOptions;
    hiddenOptions.category = DebugDrawCategory::Lighting;
    service->SetCategoryEnabled(DebugDrawCategory::Lighting, false);

    ASSERT_TRUE(service->SubmitPoint({ 0.0f, 0.0f, 0.0f }));
    ASSERT_TRUE(service->SubmitLine({ 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }));
    ASSERT_TRUE(service->SubmitTriangle(
        { 0.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f },
        { 1.0f, 0.0f, 0.0f }));
    ASSERT_TRUE(service->SubmitLine({ 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f }, hiddenOptions));

    RecordingDebugDrawPass pass(renderer);
    pass.ExecuteForTest(NLS::Render::Data::PipelineState{});

    ASSERT_EQ(pass.recordedPrimitives.size(), 3u);
    EXPECT_EQ(pass.recordedPrimitives[0].type, DebugDrawPrimitiveType::Point);
    EXPECT_EQ(pass.recordedPrimitives[1].type, DebugDrawPrimitiveType::Line);
    EXPECT_EQ(pass.recordedPrimitives[2].type, DebugDrawPrimitiveType::Triangle);
    EXPECT_EQ(pass.recordedPrimitives[1].points[0], (NLS::Maths::Vector3{ 0.0f, 0.0f, 0.0f }));
    EXPECT_EQ(pass.recordedPrimitives[1].points[1], (NLS::Maths::Vector3{ 1.0f, 0.0f, 0.0f }));
}
