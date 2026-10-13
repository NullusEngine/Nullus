#pragma once

#include "Rendering/Context/RenderScenePackage.h"

namespace NLS::Render::Context
{
    enum class RenderScenePackageBuildMode : uint8_t
    {
        BuildDefaultPassInputs = 0,
        SkipDefaultPassInputs
    };

    NLS_RENDER_API RenderScenePackage BuildRenderFramePackage(const RenderFramePacket& packet);

    NLS_RENDER_API uint64_t CountVisibleUiDrawCommands(const UI::UiDrawDataSnapshot& snapshot);

    NLS_RENDER_API RenderScenePackage BuildRenderScenePackage(
        const RenderFramePacket& packet,
        const RenderSceneBuildData& scene,
        RenderScenePackageBuildMode buildMode = RenderScenePackageBuildMode::BuildDefaultPassInputs);

    NLS_RENDER_API RenderScenePackage BuildRenderScenePackage(
        const RenderFramePacket& packet,
        RenderSceneBuildData&& scene,
        RenderScenePackageBuildMode buildMode = RenderScenePackageBuildMode::BuildDefaultPassInputs);

    NLS_RENDER_API bool AttachUiOverlaySnapshotToRenderScenePackage(
        RenderScenePackage& package,
        std::shared_ptr<const UI::UiDrawDataSnapshot> snapshot);
}
