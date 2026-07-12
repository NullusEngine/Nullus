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

    // Reserves three hardware execution lanes for editor, render, and driver work,
    // then shares the bounded remainder between foreground and artifact jobs.
    EditorJobWorkerBudget ResolveEditorJobWorkerBudget(
        uint32_t hardwareConcurrency,
        std::optional<uint32_t> backgroundWorkerOverride = std::nullopt);
}
