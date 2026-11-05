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

    // Reserves three named execution lanes before the rendering mode is known,
    // then splits one shared compute pool and the isolated blocking-I/O pool.
    EditorJobWorkerBudget ResolveEditorJobWorkerBudget(
        uint32_t hardwareConcurrency,
        std::optional<uint32_t> backgroundWorkerOverride = std::nullopt);
}
