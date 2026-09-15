#pragma once

#include "Inspector/InspectorTypes.h"

namespace NLS::Editor::Settings
{
class EditorSettingsRegistry;
}

namespace NLS::Editor::Inspector
{
/**
 * Bridges the settings modal into the common Inspector pipeline.  Selection
 * identity remains the setting id; the registry is only resolved while a
 * property is read or written, so PropertyNodes never retain a settings
 * object pointer across frames.
 */
class ProjectSettingsTargetAdapter final : public IInspectorTargetAdapter
{
public:
    static void BindRegistry(Settings::EditorSettingsRegistry& registry);
    static void UnbindRegistry(const Settings::EditorSettingsRegistry& registry);

    [[nodiscard]] bool CanBuild(const InspectorTargetSet& targets) const override;
    [[nodiscard]] uint64_t GetStructureVersion(const InspectorTargetSet& targets) const override;
    InspectorPresentation BuildPresentation(const InspectorTargetSet& targets, const InspectorBuildContext& context) const override;
};
} // namespace NLS::Editor::Inspector
