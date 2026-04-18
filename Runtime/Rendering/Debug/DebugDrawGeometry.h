#pragma once

#include <array>

#include "Rendering/Debug/DebugDrawTypes.h"
#include "RenderDef.h"

namespace NLS::Render::Entities
{
    class Light;
}

namespace NLS::Render::Debug
{
    class DebugDrawService;

    bool NLS_RENDER_API SubmitBox(
        DebugDrawService& service,
        const Maths::Vector3& position,
        const Maths::Quaternion& rotation,
        const Maths::Vector3& halfExtents,
        const DebugDrawSubmitOptions& options = {});

    bool NLS_RENDER_API SubmitSphere(
        DebugDrawService& service,
        const Maths::Vector3& position,
        const Maths::Quaternion& rotation,
        float radius,
        const DebugDrawSubmitOptions& options = {});

    bool NLS_RENDER_API SubmitCapsule(
        DebugDrawService& service,
        const Maths::Vector3& position,
        const Maths::Quaternion& rotation,
        float radius,
        float height,
        const DebugDrawSubmitOptions& options = {});

    bool NLS_RENDER_API SubmitCone(
        DebugDrawService& service,
        const Maths::Vector3& position,
        const Maths::Quaternion& rotation,
        float radius,
        float length,
        const DebugDrawSubmitOptions& options = {});

    bool NLS_RENDER_API SubmitFrustum(
        DebugDrawService& service,
        const std::array<Maths::Vector3, 8u>& corners,
        const DebugDrawSubmitOptions& options = {});

    bool NLS_RENDER_API SubmitLightVolume(
        DebugDrawService& service,
        const Entities::Light& light,
        const DebugDrawSubmitOptions& options = {});
}
