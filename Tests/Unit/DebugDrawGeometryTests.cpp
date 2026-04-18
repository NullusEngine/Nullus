#include <array>

#include <gtest/gtest.h>

#include "Math/Transform.h"
#include "Rendering/Debug/DebugDrawGeometry.h"
#include "Rendering/Debug/DebugDrawService.h"
#include "Rendering/Entities/Light.h"

TEST(DebugDrawGeometryTests, HelpersExpandIntoTheSharedPrimitiveQueue)
{
    using namespace NLS::Render::Debug;

    DebugDrawService service(64u);

    DebugDrawSubmitOptions options;
    options.category = DebugDrawCategory::Camera;
    options.style.color = { 1.0f, 1.0f, 1.0f };

    ASSERT_TRUE(SubmitBox(
        service,
        { 0.0f, 0.0f, 0.0f },
        NLS::Maths::Quaternion::Identity,
        { 1.0f, 2.0f, 3.0f },
        options));
    EXPECT_EQ(service.GetQueuedPrimitiveCount(), 12u);

    const std::array<NLS::Maths::Vector3, 8u> frustumCorners = {
        NLS::Maths::Vector3{ -1.0f, 1.0f, 1.0f },
        NLS::Maths::Vector3{ 1.0f, 1.0f, 1.0f },
        NLS::Maths::Vector3{ -1.0f, -1.0f, 1.0f },
        NLS::Maths::Vector3{ 1.0f, -1.0f, 1.0f },
        NLS::Maths::Vector3{ -2.0f, 2.0f, 5.0f },
        NLS::Maths::Vector3{ 2.0f, 2.0f, 5.0f },
        NLS::Maths::Vector3{ -2.0f, -2.0f, 5.0f },
        NLS::Maths::Vector3{ 2.0f, -2.0f, 5.0f }
    };

    ASSERT_TRUE(SubmitFrustum(service, frustumCorners, options));
    EXPECT_EQ(service.GetQueuedPrimitiveCount(), 24u);

    NLS::Maths::Transform lightTransform;
    NLS::Render::Entities::Light directionalLight(&lightTransform);
    directionalLight.type = NLS::Render::Settings::ELightType::DIRECTIONAL;

    options.category = DebugDrawCategory::Lighting;
    ASSERT_TRUE(SubmitLightVolume(service, directionalLight, options));
    EXPECT_EQ(service.GetQueuedPrimitiveCount(), 25u);

    auto visiblePrimitives = service.CollectVisiblePrimitives();
    ASSERT_EQ(visiblePrimitives.size(), 25u);
    EXPECT_EQ(visiblePrimitives.back().get().type, DebugDrawPrimitiveType::Line);
    EXPECT_EQ(visiblePrimitives.back().get().options.category, DebugDrawCategory::Lighting);
}
