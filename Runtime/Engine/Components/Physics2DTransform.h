#pragma once

#include "Components/TransformComponent.h"
#include <Math/Vector2.h>
#include <Math/Vector4.h>
#include <cmath>
#include <string>

namespace NLS::Engine::Components::Physics2DDetail
{
inline constexpr float Pi = 3.14159265358979323846f;
inline bool SameAngle(float a, float b)
{ return std::abs(std::remainder(a - b, 2 * Pi)) < 1e-6f; }
inline bool Finite(Maths::Vector2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
inline Maths::Vector2 Rotate(Maths::Vector2 v, float angle)
{
    const float c = std::cos(angle), s = std::sin(angle);
    return {c * v.x - s * v.y, s * v.x + c * v.y};
}
inline bool Same(Maths::Vector2 a, Maths::Vector2 b)
{ return std::abs(a.x - b.x) < 1e-6f && std::abs(a.y - b.y) < 1e-6f; }
inline bool Same(Maths::Vector3 a, Maths::Vector3 b)
{ return Same(Maths::Vector2{a.x, a.y}, Maths::Vector2{b.x, b.y}) && std::abs(a.z - b.z) < 1e-6f; }
inline bool Same(Maths::Quaternion a, Maths::Quaternion b)
{ return std::abs(a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w) > 1 - 1e-6f; }
struct Frame
{
    Maths::Vector2 position, xAxis, yAxis;
    float angle = 0;
};
inline bool Capture(const TransformComponent& transform, Frame& frame, std::string& error)
{
    const auto& m = transform.GetWorldMatrix();
    const auto r0 = m.GetRow0(), r1 = m.GetRow1(), r2 = m.GetRow2();
    frame.xAxis = {r0.x, r1.x}; frame.yAxis = {r0.y, r1.y};
    const auto& p = transform.GetWorldPosition();
    const auto& q = transform.GetWorldRotation();
    frame.position = {p.x, p.y};
    const float lx = std::hypot(frame.xAxis.x, frame.xAxis.y), ly = std::hypot(frame.yAxis.x, frame.yAxis.y);
    if (!Finite(frame.position) || !Finite(frame.xAxis) || !Finite(frame.yAxis) ||
        !std::isfinite(p.z) || !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w) ||
        lx < 1e-6f || ly < 1e-6f || std::abs(r2.x) > 1e-5f || std::abs(r2.y) > 1e-5f ||
        std::abs(r0.z) > 1e-5f || std::abs(r1.z) > 1e-5f ||
        std::abs(frame.xAxis.x * frame.yAxis.x + frame.xAxis.y * frame.yAxis.y) > 1e-4f * lx * ly)
    { error = "2D physics requires finite, nondegenerate XY transforms without tilt or shear."; return false; }
    frame.angle = std::atan2(2 * (q.w*q.z + q.x*q.y), 1 - 2 * (q.y*q.y + q.z*q.z));
    error.clear(); return true;
}
}
