#include <gtest/gtest.h>

#include "Math/Matrix4.h"
#include "Math/Quaternion.h"
#include "Math/Transform.h"
#include "Math/Vector3.h"
#include "Math/Vector4.h"

namespace
{
using NLS::Maths::Matrix4;
using NLS::Maths::Quaternion;
using NLS::Maths::Transform;
using NLS::Maths::Vector3;
using NLS::Maths::Vector4;

constexpr float kMathTolerance = 0.0001f;

void ExpectVector3Near(const Vector3& actual, const Vector3& expected, float tolerance = kMathTolerance)
{
    EXPECT_NEAR(actual.x, expected.x, tolerance);
    EXPECT_NEAR(actual.y, expected.y, tolerance);
    EXPECT_NEAR(actual.z, expected.z, tolerance);
}

void ExpectQuaternionNear(const Quaternion& actual, const Quaternion& expected, float tolerance = kMathTolerance)
{
    EXPECT_NEAR(actual.x, expected.x, tolerance);
    EXPECT_NEAR(actual.y, expected.y, tolerance);
    EXPECT_NEAR(actual.z, expected.z, tolerance);
    EXPECT_NEAR(actual.w, expected.w, tolerance);
}
} // namespace

TEST(MathsRuntimeTests, Vector3OperationsProduceExpectedResults)
{
    const Vector3 cross = Vector3::Cross(Vector3::Right, Vector3::Up);
    const float dot = Vector3::Dot(Vector3::Right, Vector3::Up);
    const Vector3 normalized = Vector3::Normalize(Vector3(3.0f, 0.0f, 4.0f));
    const Vector3 lerped = Vector3::Lerp(Vector3::Zero, Vector3(10.0f, 20.0f, 30.0f), 0.25f);

    ExpectVector3Near(cross, Vector3::Forward);
    EXPECT_FLOAT_EQ(dot, 0.0f);
    ExpectVector3Near(normalized, Vector3(0.6f, 0.0f, 0.8f));
    ExpectVector3Near(lerped, Vector3(2.5f, 5.0f, 7.5f));
}

TEST(MathsRuntimeTests, QuaternionRotationAndInterpolationBehaveAsExpected)
{
    const Quaternion quarterTurn(Vector3(0.0f, 0.0f, 90.0f));
    const Vector3 rotated = Quaternion::RotatePoint(Vector3::Right, quarterTurn);
    const Vector3 euler = Quaternion::EulerAngles(quarterTurn);

    ExpectVector3Near(rotated, Vector3::Up, 0.001f);
    EXPECT_NEAR(euler.x, 0.0f, 0.01f);
    EXPECT_NEAR(euler.y, 0.0f, 0.01f);
    EXPECT_NEAR(euler.z, 90.0f, 0.01f);

    const Quaternion halfTurn(Vector3(0.0f, 0.0f, 180.0f));
    const Quaternion nlerped = Quaternion::Nlerp(Quaternion::Identity, halfTurn, 0.5f);
    EXPECT_TRUE(Quaternion::IsNormalized(nlerped));

    const Quaternion normalized = Quaternion::Normalize(Quaternion(0.0f, 0.0f, 0.0f, 2.0f));
    ExpectQuaternionNear(normalized, Quaternion::Identity);
}

TEST(MathsRuntimeTests, Matrix4TranslationScalingAndInverseBehaveAsExpected)
{
    const Matrix4 translation = Matrix4::Translation(Vector3(1.0f, 2.0f, 3.0f));
    const Vector4 translated = translation * Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_NEAR(translated.x, 2.0f, kMathTolerance);
    EXPECT_NEAR(translated.y, 3.0f, kMathTolerance);
    EXPECT_NEAR(translated.z, 4.0f, kMathTolerance);
    EXPECT_NEAR(translated.w, 1.0f, kMathTolerance);

    const Matrix4 scaling = Matrix4::Scaling(Vector3(2.0f, 3.0f, 4.0f));
    const Vector4 scaled = scaling * Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_NEAR(scaled.x, 2.0f, kMathTolerance);
    EXPECT_NEAR(scaled.y, 3.0f, kMathTolerance);
    EXPECT_NEAR(scaled.z, 4.0f, kMathTolerance);
    EXPECT_NEAR(scaled.w, 1.0f, kMathTolerance);

    const Matrix4 inverse = Matrix4::Inverse(translation);
    const Vector4 backToOrigin = inverse * translated;
    EXPECT_NEAR(backToOrigin.x, 1.0f, kMathTolerance);
    EXPECT_NEAR(backToOrigin.y, 1.0f, kMathTolerance);
    EXPECT_NEAR(backToOrigin.z, 1.0f, kMathTolerance);
    EXPECT_NEAR(backToOrigin.w, 1.0f, kMathTolerance);
}

TEST(MathsRuntimeTests, TransformKeepsLocalAndWorldStateInSyncWithoutParent)
{
    Transform transform;

    transform.SetLocalPosition(Vector3(1.0f, 2.0f, 3.0f));
    ExpectVector3Near(transform.GetLocalPosition(), Vector3(1.0f, 2.0f, 3.0f), 0.001f);
    ExpectVector3Near(transform.GetWorldPosition(), Vector3(1.0f, 2.0f, 3.0f), 0.001f);

    transform.SetWorldPosition(Vector3(4.0f, 5.0f, 6.0f));
    ExpectVector3Near(transform.GetWorldPosition(), Vector3(4.0f, 5.0f, 6.0f), 0.001f);
    ExpectVector3Near(transform.GetLocalPosition(), Vector3(4.0f, 5.0f, 6.0f), 0.001f);

    transform.ScaleLocal(Vector3(2.0f, 3.0f, 4.0f));
    ExpectVector3Near(transform.GetLocalScale(), Vector3(2.0f, 3.0f, 4.0f), 0.001f);
    ExpectVector3Near(transform.GetWorldScale(), Vector3(2.0f, 3.0f, 4.0f), 0.001f);
}
