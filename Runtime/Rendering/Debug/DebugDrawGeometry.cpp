#include "Rendering/Debug/DebugDrawGeometry.h"

#include <cmath>

#include "Rendering/Debug/DebugDrawService.h"
#include "Rendering/Entities/Light.h"
#include "Rendering/Settings/ELightType.h"

namespace NLS::Render::Debug
{
namespace
{
    constexpr float kDegreesToRadians = 3.1415926535f / 180.0f;
    constexpr float kDirectionalLightHelperLength = 5.0f;

    Maths::Vector3 RotatePoint(const Maths::Quaternion& rotation, const Maths::Vector3& point)
    {
        return rotation * point;
    }

    bool SubmitLineSet(DebugDrawService& service, const std::initializer_list<std::pair<Maths::Vector3, Maths::Vector3>>& lines, const DebugDrawSubmitOptions& options)
    {
        for (const auto& [start, end] : lines)
        {
            if (!service.SubmitLine(start, end, options))
                return false;
        }

        return true;
    }
}

bool SubmitBox(
    DebugDrawService& service,
    const Maths::Vector3& position,
    const Maths::Quaternion& rotation,
    const Maths::Vector3& halfExtents,
    const DebugDrawSubmitOptions& options)
{
    const auto p000 = position + RotatePoint(rotation, { -halfExtents.x, -halfExtents.y, -halfExtents.z });
    const auto p001 = position + RotatePoint(rotation, { -halfExtents.x, -halfExtents.y, +halfExtents.z });
    const auto p010 = position + RotatePoint(rotation, { -halfExtents.x, +halfExtents.y, -halfExtents.z });
    const auto p011 = position + RotatePoint(rotation, { -halfExtents.x, +halfExtents.y, +halfExtents.z });
    const auto p100 = position + RotatePoint(rotation, { +halfExtents.x, -halfExtents.y, -halfExtents.z });
    const auto p101 = position + RotatePoint(rotation, { +halfExtents.x, -halfExtents.y, +halfExtents.z });
    const auto p110 = position + RotatePoint(rotation, { +halfExtents.x, +halfExtents.y, -halfExtents.z });
    const auto p111 = position + RotatePoint(rotation, { +halfExtents.x, +halfExtents.y, +halfExtents.z });

    return SubmitLineSet(
        service,
        {
            { p000, p001 }, { p010, p011 }, { p000, p010 }, { p001, p011 },
            { p100, p101 }, { p110, p111 }, { p100, p110 }, { p101, p111 },
            { p000, p100 }, { p010, p110 }, { p001, p101 }, { p011, p111 }
        },
        options);
}

bool SubmitSphere(
    DebugDrawService& service,
    const Maths::Vector3& position,
    const Maths::Quaternion& rotation,
    const float radius,
    const DebugDrawSubmitOptions& options)
{
    if (std::isinf(radius))
        return true;

    for (float degrees = 0.0f; degrees <= 360.0f; degrees += 10.0f)
    {
        const float radians = degrees * kDegreesToRadians;
        const float nextRadians = (degrees + 10.0f) * kDegreesToRadians;

        if (!service.SubmitLine(
                position + RotatePoint(rotation, Maths::Vector3{ std::cos(radians), std::sin(radians), 0.0f } * radius),
                position + RotatePoint(rotation, Maths::Vector3{ std::cos(nextRadians), std::sin(nextRadians), 0.0f } * radius),
                options))
            return false;

        if (!service.SubmitLine(
                position + RotatePoint(rotation, Maths::Vector3{ 0.0f, std::sin(radians), std::cos(radians) } * radius),
                position + RotatePoint(rotation, Maths::Vector3{ 0.0f, std::sin(nextRadians), std::cos(nextRadians) } * radius),
                options))
            return false;

        if (!service.SubmitLine(
                position + RotatePoint(rotation, Maths::Vector3{ std::cos(radians), 0.0f, std::sin(radians) } * radius),
                position + RotatePoint(rotation, Maths::Vector3{ std::cos(nextRadians), 0.0f, std::sin(nextRadians) } * radius),
                options))
            return false;
    }

    return true;
}

bool SubmitCapsule(
    DebugDrawService& service,
    const Maths::Vector3& position,
    const Maths::Quaternion& rotation,
    const float radius,
    const float height,
    const DebugDrawSubmitOptions& options)
{
    if (std::isinf(radius))
        return true;

    const float halfHeight = height / 2.0f;
    const Maths::Vector3 hVec = { 0.0f, halfHeight, 0.0f };

    for (float degrees = 0.0f; degrees < 360.0f; degrees += 10.0f)
    {
        const float radians = degrees * kDegreesToRadians;
        const float nextRadians = (degrees + 10.0f) * kDegreesToRadians;

        if (!service.SubmitLine(
                position + RotatePoint(rotation, hVec + Maths::Vector3{ std::cos(radians), 0.0f, std::sin(radians) } * radius),
                position + RotatePoint(rotation, hVec + Maths::Vector3{ std::cos(nextRadians), 0.0f, std::sin(nextRadians) } * radius),
                options))
            return false;

        if (!service.SubmitLine(
                position + RotatePoint(rotation, -hVec + Maths::Vector3{ std::cos(radians), 0.0f, std::sin(radians) } * radius),
                position + RotatePoint(rotation, -hVec + Maths::Vector3{ std::cos(nextRadians), 0.0f, std::sin(nextRadians) } * radius),
                options))
            return false;
    }

    return SubmitLineSet(
        service,
        {
            {
                position + RotatePoint(rotation, { -radius, -halfHeight, 0.0f }),
                position + RotatePoint(rotation, { -radius, +halfHeight, 0.0f })
            },
            {
                position + RotatePoint(rotation, { +radius, -halfHeight, 0.0f }),
                position + RotatePoint(rotation, { +radius, +halfHeight, 0.0f })
            },
            {
                position + RotatePoint(rotation, { 0.0f, -halfHeight, -radius }),
                position + RotatePoint(rotation, { 0.0f, +halfHeight, -radius })
            },
            {
                position + RotatePoint(rotation, { 0.0f, -halfHeight, +radius }),
                position + RotatePoint(rotation, { 0.0f, +halfHeight, +radius })
            }
        },
        options);
}

bool SubmitCone(
    DebugDrawService& service,
    const Maths::Vector3& position,
    const Maths::Quaternion& rotation,
    const float radius,
    const float length,
    const DebugDrawSubmitOptions& options)
{
    const auto tip = position;
    const auto baseCenter = position + RotatePoint(rotation, { 0.0f, 0.0f, length });
    constexpr float kStepDegrees = 30.0f;

    Maths::Vector3 firstBasePoint = baseCenter + RotatePoint(rotation, { radius, 0.0f, 0.0f });
    Maths::Vector3 previousBasePoint = firstBasePoint;

    for (float degrees = kStepDegrees; degrees <= 360.0f; degrees += kStepDegrees)
    {
        const float radians = degrees * kDegreesToRadians;
        const Maths::Vector3 currentBasePoint = baseCenter + RotatePoint(rotation, { std::cos(radians) * radius, std::sin(radians) * radius, 0.0f });

        if (!service.SubmitLine(previousBasePoint, currentBasePoint, options))
            return false;

        previousBasePoint = currentBasePoint;
    }

    return SubmitLineSet(
        service,
        {
            { tip, firstBasePoint },
            { tip, baseCenter + RotatePoint(rotation, { -radius, 0.0f, 0.0f }) },
            { tip, baseCenter + RotatePoint(rotation, { 0.0f, radius, 0.0f }) },
            { tip, baseCenter + RotatePoint(rotation, { 0.0f, -radius, 0.0f }) }
        },
        options);
}

bool SubmitFrustum(
    DebugDrawService& service,
    const std::array<Maths::Vector3, 8u>& corners,
    const DebugDrawSubmitOptions& options)
{
    const auto& a = corners[0];
    const auto& b = corners[1];
    const auto& c = corners[2];
    const auto& d = corners[3];
    const auto& e = corners[4];
    const auto& f = corners[5];
    const auto& g = corners[6];
    const auto& h = corners[7];

    return SubmitLineSet(
        service,
        {
            { a, b }, { b, d }, { d, c }, { c, a },
            { e, f }, { f, h }, { h, g }, { g, e },
            { a, e }, { b, f }, { c, g }, { d, h }
        },
        options);
}

bool SubmitLightVolume(
    DebugDrawService& service,
    const Entities::Light& light,
    const DebugDrawSubmitOptions& options)
{
    auto lightOptions = options;
    lightOptions.category = DebugDrawCategory::Lighting;

    const auto position = light.transform->GetWorldPosition();
    const auto rotation = light.transform->GetWorldRotation();

    switch (light.type)
    {
    case Settings::ELightType::POINT:
        return SubmitSphere(service, position, rotation, light.GetEffectRange(), lightOptions);

    case Settings::ELightType::DIRECTIONAL:
        return service.SubmitLine(position, position + light.transform->GetWorldForward() * kDirectionalLightHelperLength, lightOptions);

    case Settings::ELightType::SPOT:
        return SubmitCone(service, position, rotation, light.GetEffectRange(), light.GetEffectRange(), lightOptions);

    case Settings::ELightType::AMBIENT_BOX:
        return SubmitBox(service, position, rotation, { light.constant, light.linear, light.quadratic }, lightOptions);

    case Settings::ELightType::AMBIENT_SPHERE:
        return SubmitSphere(service, position, rotation, light.constant, lightOptions);
    }

    return true;
}
}
