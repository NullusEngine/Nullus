#pragma once

#include "EngineDef.h"
#include "UICommon.h"

#include <cstddef>
#include <span>

namespace NLS::Engine::UI
{
    struct UIVector3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        constexpr UIVector3 operator+(const UIVector3& rhs) const { return { x + rhs.x, y + rhs.y, z + rhs.z }; }
        constexpr UIVector3 operator-(const UIVector3& rhs) const { return { x - rhs.x, y - rhs.y, z - rhs.z }; }
        constexpr UIVector3 operator*(float scalar) const { return { x * scalar, y * scalar, z * scalar }; }
    };

    struct UIRay3
    {
        UIVector3 origin;
        UIVector3 direction;
    };

    struct UIWorldTriangle
    {
        UIVector3 a;
        UIVector3 b;
        UIVector3 c;
        UIVector2 uvA;
        UIVector2 uvB;
        UIVector2 uvC;
    };

    struct NLS_ENGINE_API UIWorldHitResult
    {
        bool hit = false;
        float distance = 0.0f;
        UIVector2 localPosition;
        UIVector2 uv;
        size_t triangleIndex = static_cast<size_t>(-1);
    };

    NLS_ENGINE_API bool IntersectWorldCanvasPlane(
        const UIRay3& ray,
        UIVector3 origin,
        UIVector3 normal,
        UIVector3 right,
        UIVector3 up,
        UIVector2 canvasSize,
        UIWorldHitResult& result);

    NLS_ENGINE_API bool IntersectWorldCanvasTriangles(
        const UIRay3& ray,
        std::span<const UIWorldTriangle> triangles,
        UIWorldHitResult& result);
}
