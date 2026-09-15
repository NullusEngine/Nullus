#pragma once

#include <memory>

#include "Serialization/SerializedObject.h"

namespace NLS::Editor::Settings
{
class EditorSettingObject;
class EditorSettingsRegistry;
}

namespace NLS::Editor::Serialization
{
// Resolves stable Project Settings ids to a byte-stream target. The registry
// is target resolution only; PropertyNode callbacks are never involved.
class ProjectSettingsSerializedBackend final : public ISerializedTargetBackend
{
public:
    [[nodiscard]] static std::shared_ptr<ProjectSettingsSerializedBackend> Create();
    [[nodiscard]] static bool Supports(const SerializedTargetSet& targets);

    static void BindRegistry(Settings::EditorSettingsRegistry& registry);
    static void UnbindRegistry(const Settings::EditorSettingsRegistry& registry);
    [[nodiscard]] static const Settings::EditorSettingObject* ResolveSetting(
        const SerializedTargetHandle& target);
    [[nodiscard]] static uint64_t GetRegistryRevision();

    NLS::Engine::Serialize::TypeTreeResult BuildTypeTree(
        const SerializedTargetSet& targets,
        SerializedInspectorMode mode) const override;
    NLS::Engine::Serialize::StreamResult SerializeTarget(
        const SerializedTargetHandle& target,
        std::shared_ptr<const NLS::Engine::Serialize::SerializedTypeTree> schema,
        NLS::Engine::Serialize::SerializedReferenceTable& references) const override;
    ValidationResult ValidateAndNormalize(
        const SerializedTargetHandle& target,
        std::span<const SerializedPropertyPath> modifiedPaths,
        NLS::Engine::Serialize::SerializedByteStream& stream) const override;
    ApplyResult DeserializeTarget(
        const SerializedTargetHandle& target,
        const NLS::Engine::Serialize::SerializedByteStream& stream,
        const Undo::UndoExecutionContext& context) const override;
    SerializedVersionStamp GetVersion(const SerializedTargetHandle& target) const override;
};
} // namespace NLS::Editor::Serialization
