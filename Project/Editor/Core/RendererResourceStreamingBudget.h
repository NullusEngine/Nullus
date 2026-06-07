#pragma once

#include <chrono>
#include <cstddef>

namespace NLS::Editor::Core
{
    struct PrefabRendererResourceStreamingBudget
    {
        std::chrono::milliseconds frameBudget;
        size_t resourcePrewarmsPerFrame = 0u;
        size_t meshPrewarmsPerFrame = 0u;
        size_t materialPrewarmsPerFrame = 0u;
        size_t textureCompletionsPerFrame = 0u;
        size_t meshBindsPerFrame = 0u;
        size_t maxInflightMeshLoads = 0u;
    };

    inline constexpr PrefabRendererResourceStreamingBudget kPrefabRendererResourceStreamingBudget {
        std::chrono::milliseconds(12),
        64u,
        32u,
        32u,
        64u,
        32u,
        64u
    };

    inline constexpr PrefabRendererResourceStreamingBudget GetSceneLoadPrefabRendererResourceStreamingBudget()
    {
        return kPrefabRendererResourceStreamingBudget;
    }

    inline constexpr PrefabRendererResourceStreamingBudget GetDragPreviewPrefabRendererResourceStreamingBudget()
    {
        return kPrefabRendererResourceStreamingBudget;
    }

    inline constexpr auto kPrefabRendererResourceStreamingFrameBudget =
        kPrefabRendererResourceStreamingBudget.frameBudget;
    inline constexpr size_t kPrefabRendererResourcePrewarmsPerFrame =
        kPrefabRendererResourceStreamingBudget.resourcePrewarmsPerFrame;
    inline constexpr size_t kPrefabRendererResourceMeshPrewarmsPerFrame =
        kPrefabRendererResourceStreamingBudget.meshPrewarmsPerFrame;
    inline constexpr size_t kPrefabRendererResourceMaterialPrewarmsPerFrame =
        kPrefabRendererResourceStreamingBudget.materialPrewarmsPerFrame;
    inline constexpr size_t kPrefabRendererResourceTextureCompletionsPerFrame =
        kPrefabRendererResourceStreamingBudget.textureCompletionsPerFrame;
    inline constexpr size_t kPrefabRendererResourceMeshBindsPerFrame =
        kPrefabRendererResourceStreamingBudget.meshBindsPerFrame;
    inline constexpr size_t kPrefabRendererResourceMaxInflightMeshLoads =
        kPrefabRendererResourceStreamingBudget.maxInflightMeshLoads;
}
