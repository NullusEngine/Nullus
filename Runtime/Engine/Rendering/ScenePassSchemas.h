#pragma once

#include <cstdint>

#include "Rendering/Context/ThreadedRenderingLifecycle.h"

namespace NLS::Engine::Rendering
{
    enum class ForwardScenePassPipelineKind : uint8_t
    {
        Default = 0,
        Skybox
    };

    enum class ForwardScenePassExecutionKind : uint8_t
    {
        Unknown = 0,
        Opaque,
        Skybox,
        Transparent
    };

    enum class DeferredScenePassExecutionKind : uint8_t
    {
        Unknown = 0,
        GBuffer,
        Lighting
    };

    inline ForwardScenePassExecutionKind GetForwardScenePassExecutionKind(
        const NLS::Render::Context::RenderPassCommandKind kind)
    {
        switch (kind)
        {
        case NLS::Render::Context::RenderPassCommandKind::Opaque:
            return ForwardScenePassExecutionKind::Opaque;
        case NLS::Render::Context::RenderPassCommandKind::Skybox:
            return ForwardScenePassExecutionKind::Skybox;
        case NLS::Render::Context::RenderPassCommandKind::Transparent:
            return ForwardScenePassExecutionKind::Transparent;
        default:
            return ForwardScenePassExecutionKind::Unknown;
        }
    }

    inline ForwardScenePassPipelineKind GetForwardScenePassPipelineKind(
        const NLS::Render::Context::RenderPassCommandKind kind)
    {
        return GetForwardScenePassExecutionKind(kind) == ForwardScenePassExecutionKind::Skybox
            ? ForwardScenePassPipelineKind::Skybox
            : ForwardScenePassPipelineKind::Default;
    }

    inline DeferredScenePassExecutionKind GetDeferredScenePassExecutionKind(
        const NLS::Render::Context::RenderPassCommandKind kind)
    {
        switch (kind)
        {
        case NLS::Render::Context::RenderPassCommandKind::GBuffer:
            return DeferredScenePassExecutionKind::GBuffer;
        case NLS::Render::Context::RenderPassCommandKind::Lighting:
            return DeferredScenePassExecutionKind::Lighting;
        default:
            return DeferredScenePassExecutionKind::Unknown;
        }
    }
}
