#pragma once

#include <cstdint>
#include <optional>

namespace NLS::Editor::Core
{
    struct EditorJobWorkerBudget
    {
        uint32_t foregroundWorkerCount = 1u;
        uint32_t backgroundWorkerCount = 2u;
    };

    // Reserves three hardware execution lanes for the game, render, and RHI
    // threads, hands half of the bounded remainder to the rendering TaskGraph,
    // and returns this editor's share of the Jobs pool.
    EditorJobWorkerBudget ResolveEditorJobWorkerBudget(
        uint32_t hardwareConcurrency,
        std::optional<uint32_t> backgroundWorkerOverride = std::nullopt);
}
