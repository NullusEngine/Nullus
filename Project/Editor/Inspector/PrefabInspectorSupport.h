#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "Inspector/InspectorTypes.h"

namespace NLS::Editor::Assets
{
class PrefabInstanceRegistry;
}

namespace NLS::Editor::Inspector
{
// The Inspector keeps only stable target handles. This service resolves the
// live prefab instance when a menu is opened or an action is executed.
enum class PrefabInspectorActionKind : uint8_t
{
    Apply,
    Revert
};

struct PrefabInspectorAction
{
    PrefabInspectorActionKind kind = PrefabInspectorActionKind::Apply;
    std::string label;
    std::string tooltip;
    bool enabled = false;
};

// Re-evaluate override presentation without rebuilding field serialization.
// This keeps ordinary property previews from allocating a new widget/model
// tree while still updating prefab markers after an edit transaction ends.
void RefreshPrefabOverrideStates(InspectorPresentation& presentation);

[[nodiscard]] PrefabOverrideState GetPrefabOverrideState(const PropertyNode& node);
[[nodiscard]] std::vector<PrefabInspectorAction> GetPrefabInspectorActions(const PropertyNode& node);
[[nodiscard]] std::vector<PrefabInspectorAction> GetPrefabInspectorActions(const InspectorTargetSet& targets);

bool ExecutePrefabInspectorAction(
    const PropertyNode& node,
    PrefabInspectorActionKind action,
    std::string& error);
bool ExecutePrefabInspectorAction(
    const InspectorTargetSet& targets,
    PrefabInspectorActionKind action,
    std::string& error);

#if defined(NLS_ENABLE_TEST_HOOKS)
namespace TestHooks
{
// Keeps Prefab Inspector action tests independent of the graphics-heavy editor
// context while exercising the same menu and Undo entry points as production.
class ScopedPrefabInspectorServicesForTesting final
{
public:
    explicit ScopedPrefabInspectorServicesForTesting(
        Assets::PrefabInstanceRegistry& instanceRegistry,
        std::filesystem::path projectAssetsPath = {});
    ~ScopedPrefabInspectorServicesForTesting();

    ScopedPrefabInspectorServicesForTesting(const ScopedPrefabInspectorServicesForTesting&) = delete;
    ScopedPrefabInspectorServicesForTesting& operator=(const ScopedPrefabInspectorServicesForTesting&) = delete;

private:
    Assets::PrefabInstanceRegistry* m_previousInstanceRegistry = nullptr;
    std::filesystem::path m_previousProjectAssetsPath;
};
} // namespace TestHooks
#endif
} // namespace NLS::Editor::Inspector
