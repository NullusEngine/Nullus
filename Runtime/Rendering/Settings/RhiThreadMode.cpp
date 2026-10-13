#include "Rendering/Settings/RhiThreadMode.h"

#include "Rendering/Settings/GraphicsBackendUtils.h"

#include <algorithm>
#include <cctype>

namespace NLS::Render::Settings
{
    namespace
    {
        std::string ToLowerAscii(std::string_view value)
        {
            std::string lowered(value);
            std::transform(
                lowered.begin(),
                lowered.end(),
                lowered.begin(),
                [](const unsigned char character)
                {
                    return static_cast<char>(std::tolower(character));
                });
            return lowered;
        }
    }

    std::optional<RhiThreadModePreference> ParseRhiThreadModePreference(
        const std::string_view value)
    {
        const std::string lowered = ToLowerAscii(value);
        if (lowered.empty() || lowered == "backend_default" || lowered == "default")
            return RhiThreadModePreference::BackendDefault;
        if (lowered == "none")
            return RhiThreadModePreference::ForceNone;
        if (lowered == "dedicated" || lowered == "dedicated_thread")
            return RhiThreadModePreference::ForceDedicatedThread;
        if (lowered == "tasks")
            return RhiThreadModePreference::ForceTasks;
        return std::nullopt;
    }

    const char* ToString(const RhiThreadMode mode)
    {
        switch (mode)
        {
        case RhiThreadMode::None: return "None";
        case RhiThreadMode::DedicatedThread: return "DedicatedThread";
        case RhiThreadMode::Tasks: return "Tasks";
        default: return "Unknown";
        }
    }

    const char* ToString(const RhiThreadModePreference preference)
    {
        switch (preference)
        {
        case RhiThreadModePreference::BackendDefault: return "BackendDefault";
        case RhiThreadModePreference::ForceNone: return "ForceNone";
        case RhiThreadModePreference::ForceDedicatedThread: return "ForceDedicatedThread";
        case RhiThreadModePreference::ForceTasks: return "ForceTasks";
        default: return "Unknown";
        }
    }

    RhiThreadMode GetDefaultRhiThreadMode(const RHI::NativeBackendType backend)
    {
        switch (backend)
        {
        case RHI::NativeBackendType::DX12:
        case RHI::NativeBackendType::Vulkan:
            return RhiThreadMode::Tasks;
        case RHI::NativeBackendType::Metal:
            // Metal stays on a dedicated lane until thread ownership and
            // concurrent submission are validated on device.
            return RhiThreadMode::DedicatedThread;
        default:
            return RhiThreadMode::DedicatedThread;
        }
    }

    const char* ToConfigString(const RhiThreadModePreference preference)
    {
        switch (preference)
        {
        case RhiThreadModePreference::BackendDefault: return "backend_default";
        case RhiThreadModePreference::ForceNone: return "none";
        case RhiThreadModePreference::ForceDedicatedThread: return "dedicated_thread";
        case RhiThreadModePreference::ForceTasks: return "tasks";
        default: return nullptr;
        }
    }

    bool SupportsRhiThreadMode(
        const RHI::NativeBackendType backend,
        const RHI::RHIDeviceCapabilities& capabilities,
        const RhiThreadMode mode)
    {
        (void)backend;
        switch (mode)
        {
        case RhiThreadMode::None:
        case RhiThreadMode::DedicatedThread:
            // Both are always available: they only need the render thread.
            return true;
        case RhiThreadMode::Tasks:
            return capabilities.supportsRhiTasksMode;
        default:
            return false;
        }
    }

    RhiThreadModeResolution ResolveRhiThreadMode(
        const RHI::NativeBackendType backend,
        const RHI::RHIDeviceCapabilities& capabilities,
        const RhiThreadModePreference preference)
    {
        RhiThreadModeResolution resolution;
        resolution.usedBackendDefault = preference == RhiThreadModePreference::BackendDefault;

        switch (preference)
        {
        case RhiThreadModePreference::ForceNone:
            resolution.mode = RhiThreadMode::None;
            break;
        case RhiThreadModePreference::ForceDedicatedThread:
            resolution.mode = RhiThreadMode::DedicatedThread;
            break;
        case RhiThreadModePreference::ForceTasks:
            resolution.mode = RhiThreadMode::Tasks;
            break;
        case RhiThreadModePreference::BackendDefault:
        default:
            resolution.mode = GetDefaultRhiThreadMode(backend);
            break;
        }

        const bool supported = SupportsRhiThreadMode(backend, capabilities, resolution.mode);
        resolution.accepted = supported;

        resolution.summary =
            std::string("backend=") + ToString(backend) +
            " requested=" + ToString(preference) +
            " resolved=" + ToString(resolution.mode) +
            (resolution.usedBackendDefault ? " (backend default)" : " (forced)");

        if (!supported)
        {
            resolution.summary +=
                " rejected: backend does not advertise the RHI tasks lane capability";
            const RHI::RHIDeviceFeatureState state =
                capabilities.GetFeature(RHI::RHIDeviceFeature::RhiTasksMode);
            if (!state.reason.empty())
                resolution.summary += " (" + state.reason + ")";
            return resolution;
        }

        if (!resolution.usedBackendDefault && resolution.mode == RhiThreadMode::Tasks)
            resolution.summary += " accepted";

        return resolution;
    }
}
