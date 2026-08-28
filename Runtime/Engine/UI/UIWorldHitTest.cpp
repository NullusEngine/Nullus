#include "UIWorldHitTest.h"

#include <algorithm>
#include <cmath>

namespace NLS::Engine::UI
{
    namespace
    {
        float Dot(const UIVector3 lhs, const UIVector3 rhs)
        {
            return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
        }

        UIVector3 Cross(const UIVector3 lhs, const UIVector3 rhs)
        {
            return {
                lhs.y * rhs.z - lhs.z * rhs.y,
                lhs.z * rhs.x - lhs.x * rhs.z,
                lhs.x * rhs.y - lhs.y * rhs.x };
        }

        float LengthSquared(const UIVector3 value)
        {
            return Dot(value, value);
        }

        UIVector3 Normalize(const UIVector3 value)
        {
            const auto length = std::sqrt(LengthSquared(value));
            return length > 0.000001f ? value * (1.0f / length) : UIVector3 {};
        }
    }

    bool IntersectWorldCanvasPlane(
        const UIRay3& ray,
        const UIVector3 origin,
        const UIVector3 normal,
        const UIVector3 right,
        const UIVector3 up,
        const UIVector2 canvasSize,
        UIWorldHitResult& result)
    {
        result = {};
        if (canvasSize.x <= 0.0f || canvasSize.y <= 0.0f)
            return false;
        const auto planeNormal = Normalize(normal);
        const auto denominator = Dot(ray.direction, planeNormal);
        if (std::abs(denominator) <= 0.000001f)
            return false;
        const auto distance = Dot(origin - ray.origin, planeNormal) / denominator;
        if (distance < 0.0f)
            return false;
        const auto hitPoint = ray.origin + ray.direction * distance;
        const auto relative = hitPoint - origin;
        const auto local = UIVector2 { Dot(relative, right), Dot(relative, up) };
        if (local.x < 0.0f || local.y < 0.0f || local.x > canvasSize.x || local.y > canvasSize.y)
            return false;
        result.hit = true;
        result.distance = distance;
        result.localPosition = local;
        result.uv = { local.x / canvasSize.x, local.y / canvasSize.y };
        return true;
    }

    bool IntersectWorldCanvasTriangles(
        const UIRay3& ray,
        const std::span<const UIWorldTriangle> triangles,
        UIWorldHitResult& result)
    {
        result = {};
        constexpr float epsilon = 0.000001f;
        for (size_t index = 0u; index < triangles.size(); ++index)
        {
            const auto& triangle = triangles[index];
            const auto edge1 = triangle.b - triangle.a;
            const auto edge2 = triangle.c - triangle.a;
            const auto p = Cross(ray.direction, edge2);
            const auto determinant = Dot(edge1, p);
            if (std::abs(determinant) <= epsilon)
                continue;
            const auto inverse = 1.0f / determinant;
            const auto t = ray.origin - triangle.a;
            const auto barycentricU = Dot(t, p) * inverse;
            if (barycentricU < 0.0f || barycentricU > 1.0f)
                continue;
            const auto q = Cross(t, edge1);
            const auto barycentricV = Dot(ray.direction, q) * inverse;
            if (barycentricV < 0.0f || barycentricU + barycentricV > 1.0f)
                continue;
            const auto distance = Dot(edge2, q) * inverse;
            if (distance < 0.0f || (result.hit && distance >= result.distance))
                continue;

            const auto barycentricW = 1.0f - barycentricU - barycentricV;
            result.hit = true;
            result.distance = distance;
            result.triangleIndex = index;
            result.uv = triangle.uvA * barycentricW +
                triangle.uvB * barycentricU + triangle.uvC * barycentricV;
            result.localPosition = result.uv;
        }
        return result.hit;
    }
}
