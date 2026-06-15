#pragma once

#include "Rendering/Context/ThreadedRenderingLifecycle.h"

namespace NLS::Render::Context
{
    enum class SnapshotRenderScenePackageBuildMode : uint8_t
    {
        BuildDefaultPassInputs = 0,
        SkipDefaultPassInputs
    };

    NLS_RENDER_API RenderScenePackage BuildSnapshotOwnedRenderScenePackage(
        const FrameSnapshot& snapshot,
        SnapshotRenderScenePackageBuildMode buildMode = SnapshotRenderScenePackageBuildMode::BuildDefaultPassInputs);

    NLS_RENDER_API bool AttachUiOverlaySnapshotToRenderScenePackage(
        RenderScenePackage& package,
        std::shared_ptr<const UI::UiDrawDataSnapshot> snapshot);
}
