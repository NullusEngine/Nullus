#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "RenderDef.h"
#include "Rendering/RHI/RHITypes.h"

namespace NLS::Render::Settings
{
    /**
     * How the logical RHI lane is executed. Mirrors UE's ERHIThreadMode.
     *
     * None            - the render thread submits synchronously.
     * DedicatedThread - a dedicated OS thread owns the logical RHI lane.
     * Tasks           - the logical RHI lane runs on TaskGraph workers while the
     *                   executor keeps dispatch/translate/submit ordered.
     */
    enum class NLS_RENDER_API RhiThreadMode : uint8_t
    {
        None = 0u,
        DedicatedThread,
        Tasks
    };

    /** What the caller asked for, before capability clamping. */
    enum class NLS_RENDER_API RhiThreadModePreference : uint8_t
    {
        BackendDefault = 0u,
        ForceNone,
        ForceDedicatedThread,
        ForceTasks
    };

    NLS_RENDER_API const char* ToString(RhiThreadMode mode);
    NLS_RENDER_API const char* ToString(RhiThreadModePreference preference);
    /** Canonical project setting value; null for an invalid preference. */
    NLS_RENDER_API const char* ToConfigString(RhiThreadModePreference preference);

    /**
     * Parses a restart-scoped setting value. Accepts "backend_default"/"default",
     * "none", "dedicated"/"dedicated_thread" and "tasks" (case-insensitive).
     * Returns nullopt for an unrecognised value so callers can reject it loudly.
     */
    NLS_RENDER_API std::optional<RhiThreadModePreference> ParseRhiThreadModePreference(
        std::string_view value);

    /** Backend default when the caller does not force a mode. */
    NLS_RENDER_API RhiThreadMode GetDefaultRhiThreadMode(RHI::NativeBackendType backend);

    /** Whether the backend capabilities permit the requested mode. */
    NLS_RENDER_API bool SupportsRhiThreadMode(
        RHI::NativeBackendType backend,
        const RHI::RHIDeviceCapabilities& capabilities,
        RhiThreadMode mode);

    struct NLS_RENDER_API RhiThreadModeResolution
    {
        // False means the request must be rejected: a forced mode the backend does
        // not support never silently degrades to a different scheduler.
        bool accepted = false;
        RhiThreadMode mode = RhiThreadMode::DedicatedThread;
        bool usedBackendDefault = false;
        std::string summary;

        [[nodiscard]] bool WasForced() const { return !usedBackendDefault; }
    };

    /**
     * Resolves the effective RHI thread mode and explains the decision. The
     * summary always names the backend, the requested mode and the outcome so a
     * startup failure can be diagnosed without a debugger.
     */
    NLS_RENDER_API RhiThreadModeResolution ResolveRhiThreadMode(
        RHI::NativeBackendType backend,
        const RHI::RHIDeviceCapabilities& capabilities,
        RhiThreadModePreference preference);
}
