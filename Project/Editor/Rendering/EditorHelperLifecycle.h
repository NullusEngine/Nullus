#pragma once

#include <cstdint>

#include "Rendering/Context/ThreadedRenderingLifecycle.h"

namespace NLS::Editor::Rendering
{
    struct ThreadedEditorHelperState
    {
        bool gridPassEnabled = true;
        bool cameraPassEnabled = true;
        bool lightPassEnabled = true;
        bool gameObjectPassEnabled = true;
        bool debugDrawPassEnabled = true;
        bool debugDrawEnabled = true;
        bool debugDrawCamera = true;
        bool debugDrawLighting = true;
        bool gridEnabled = false;
        uint64_t sceneCameraCount = 0u;
        uint64_t sceneLightCount = 0u;
        bool hasSelectedGameObject = false;
        bool hasVisibleDebugDrawPrimitives = false;
    };

    inline bool HasThreadedGridHelperPass(const ThreadedEditorHelperState& state)
    {
        return state.gridPassEnabled && state.gridEnabled;
    }

    inline bool HasThreadedCameraHelperPass(const ThreadedEditorHelperState& state)
    {
        return state.cameraPassEnabled &&
            state.debugDrawEnabled &&
            state.debugDrawCamera &&
            state.sceneCameraCount > 0u;
    }

    inline bool HasThreadedLightHelperPass(const ThreadedEditorHelperState& state)
    {
        return state.lightPassEnabled &&
            state.debugDrawEnabled &&
            state.debugDrawLighting &&
            state.sceneLightCount > 0u;
    }

    inline bool HasThreadedOutlineHelperPass(const ThreadedEditorHelperState& state)
    {
        return state.gameObjectPassEnabled && state.hasSelectedGameObject;
    }

    inline bool HasThreadedGizmoHelperPass(const ThreadedEditorHelperState& state)
    {
        return state.gameObjectPassEnabled && state.hasSelectedGameObject;
    }

    inline bool HasThreadedDebugDrawHelperPass(const ThreadedEditorHelperState& state)
    {
        return state.debugDrawPassEnabled && state.hasVisibleDebugDrawPrimitives;
    }

    inline uint64_t CountThreadedEditorHelperPasses(const ThreadedEditorHelperState& state)
    {
        return static_cast<uint64_t>(HasThreadedGridHelperPass(state)) +
            static_cast<uint64_t>(HasThreadedCameraHelperPass(state)) +
            static_cast<uint64_t>(HasThreadedLightHelperPass(state)) +
            static_cast<uint64_t>(HasThreadedOutlineHelperPass(state)) +
            static_cast<uint64_t>(HasThreadedGizmoHelperPass(state)) +
            static_cast<uint64_t>(HasThreadedDebugDrawHelperPass(state));
    }

    inline bool WritesThreadedEditorSceneOutput(
        const NLS::Render::Context::RenderPassCommandInput& input)
    {
        return input.usesColorAttachment && input.colorAttachmentViews.empty();
    }
}
