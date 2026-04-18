#include "Rendering/Debug/DebugDrawTypes.h"

namespace NLS::Render::Debug
{
DebugDrawLifetime DebugDrawLifetime::OneFrame()
{
    return { DebugDrawLifetimeMode::OneFrame, 1u };
}

DebugDrawLifetime DebugDrawLifetime::Frames(const uint32_t frameCount)
{
    return { DebugDrawLifetimeMode::FrameCount, frameCount };
}

DebugDrawLifetime DebugDrawLifetime::Persistent()
{
    return { DebugDrawLifetimeMode::Persistent, 0u };
}
}
