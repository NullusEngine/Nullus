#include <array>
#include <cmath>

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

TEST(DebugDrawGeometryTests, SphereHelperUsesBoundedLineBudget)
{
    using namespace NLS::Render::Debug;
    using NLS::Maths::Vector3;

    auto length = [](const Vector3& point)
    {
        return std::sqrt(point.x * point.x + point.y * point.y + point.z * point.z);
    };

    auto isNearZero = [](const float value)
    {
        return std::abs(value) <= 0.0001f;
    };

    DebugDrawService service(128u);

    DebugDrawSubmitOptions options;
    options.category = DebugDrawCategory::Bounds;

    ASSERT_TRUE(SubmitSphere(
        service,
        { 0.0f, 0.0f, 0.0f },
        NLS::Maths::Quaternion::Identity,
        1.0f,
        options));

    EXPECT_EQ(service.GetQueuedPrimitiveCount(), 72u);

    size_t xyRingSegmentCount = 0u;
    size_t yzRingSegmentCount = 0u;
    size_t xzRingSegmentCount = 0u;

    for (const auto& primitive : service.CollectVisiblePrimitives())
    {
        EXPECT_EQ(primitive.get().type, DebugDrawPrimitiveType::Line);
        EXPECT_EQ(primitive.get().options.category, DebugDrawCategory::Bounds);
        EXPECT_NEAR(length(primitive.get().points[0]), 1.0f, 0.0001f);
        EXPECT_NEAR(length(primitive.get().points[1]), 1.0f, 0.0001f);

        if (isNearZero(primitive.get().points[0].z) && isNearZero(primitive.get().points[1].z))
            ++xyRingSegmentCount;
        else if (isNearZero(primitive.get().points[0].x) && isNearZero(primitive.get().points[1].x))
            ++yzRingSegmentCount;
        else if (isNearZero(primitive.get().points[0].y) && isNearZero(primitive.get().points[1].y))
            ++xzRingSegmentCount;
        else
            ADD_FAILURE() << "Sphere segment does not lie on a principal debug ring";
    }

    EXPECT_EQ(xyRingSegmentCount, 24u);
    EXPECT_EQ(yzRingSegmentCount, 24u);
    EXPECT_EQ(xzRingSegmentCount, 24u);
}
