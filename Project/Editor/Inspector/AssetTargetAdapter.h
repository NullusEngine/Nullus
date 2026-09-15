#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "Inspector/InspectorTypes.h"

namespace NLS::Editor::Inspector
{
/**
 * Resolves importer and material targets from persistent asset identity. The
 * adapter deliberately keeps only draft settings and paths; resource pointers
 * are reacquired for every read/write operation.
 */
class AssetTargetAdapter final : public IInspectorTargetAdapter
{
public:
    [[nodiscard]] bool CanBuild(const InspectorTargetSet& targets) const override;
    [[nodiscard]] uint64_t GetStructureVersion(const InspectorTargetSet& targets) const override;
    InspectorPresentation BuildPresentation(const InspectorTargetSet& targets, const InspectorBuildContext& context) const override;
    [[nodiscard]] std::vector<InspectorTargetAction> GetActions(const InspectorTargetSet& targets) const override;
    bool ExecuteAction(
        const InspectorTargetSet& targets,
        InspectorTargetActionKind action,
        std::string& error) const override;
};

#if defined(NLS_ENABLE_TEST_HOOKS)
namespace TestHooks
{
struct ImporterCompletionStateForTesting
{
    bool available = false;
    bool dirty = false;
    bool reimportQueued = false;
    uint64_t revision = 0u;
    std::map<std::string, std::string> settings;
    std::string lastError;
};

// Exercises the same successful manual-Reimport completion path without
// requiring a background importer in unit tests.
void NotifyAssetImporterReimportedForTesting(const InspectorTargetHandle& target);

// Drives the production completion state machine with a delayed callback
// snapshot. The returned state is captured after completion and the temporary
// draft entry is removed so tests cannot leak importer state across cases.
ImporterCompletionStateForTesting CompleteAssetImporterReimportForTesting(
    const InspectorTargetHandle& target,
    uint64_t activeRevision,
    uint64_t completionRevision,
    std::map<std::string, std::string> initialSettings,
    std::optional<std::map<std::string, std::string>> appliedSettings,
    bool imported,
    std::string error);

// Seeds an in-memory importer draft for stream-backend unit tests. This is a
// target-liveness fixture only; production resolution still requires the
// asset source path and its metadata to exist in the active Editor context.
void SeedImporterDraftForTesting(
    const InspectorTargetHandle& target,
    std::map<std::string, std::string> settings);

// Removes the matching stream-backend test fixture and its transient draft.
void ClearImporterDraftForTesting(const InspectorTargetHandle& target);
}
#endif
} // namespace NLS::Editor::Inspector
