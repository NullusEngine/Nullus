#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
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

// Holds the discovered prefab override snapshot for one Inspector revision.
// Inspector controls can query as many property nodes as they need without
// re-discovering the same prefab graph for every row.
class PrefabOverrideQuery final
{
public:
    PrefabOverrideQuery();
    ~PrefabOverrideQuery();

    PrefabOverrideQuery(const PrefabOverrideQuery&) = delete;
    PrefabOverrideQuery& operator=(const PrefabOverrideQuery&) = delete;

    // Starts one marker refresh. The per-instance index and its node lookup
    // tables persist across selections; content changes call InvalidateAll()
    // explicitly so a later selection cannot reuse stale markers.
    void BeginRefresh();
    void InvalidateAll();

    [[nodiscard]] PrefabOverrideState GetState(const PropertyNode& node);
    [[nodiscard]] bool HasInstance(const InspectorTargetHandle& target) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Re-evaluate override presentation without rebuilding field serialization.
// This keeps ordinary property previews from allocating a new widget/model
// tree while still updating prefab markers after an edit transaction ends.
void RefreshPrefabOverrideStates(InspectorPresentation& presentation);
void RefreshPrefabOverrideStates(
    InspectorPresentation& presentation,
    PrefabOverrideQuery& query);

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

// Number of cold override-discovery passes performed by PrefabOverrideQuery
// since the last reset. Lets tests prove a selection refresh reused the index
// instead of rescanning the prefab graph.
[[nodiscard]] uint64_t GetPrefabOverrideMarkerDiscoveryCountForTesting();
void ResetPrefabOverrideMarkerDiscoveryCountForTesting();
} // namespace TestHooks
#endif
} // namespace NLS::Editor::Inspector
