#pragma once

#include <memory>

#include "Serialization/SerializedObject.h"

namespace NLS::Editor::Serialization
{
// Native byte-stream backend for the editable importer draft. The draft is
// intentionally separate from the persisted .meta file: property Apply only
// changes this staged editor model, while the existing Apply/Reimport action
// owns the asynchronous disk transaction.
class ImporterDraftSerializedBackend final : public ISerializedTargetBackend
{
public:
    [[nodiscard]] static std::shared_ptr<ImporterDraftSerializedBackend> Create();
    [[nodiscard]] static bool Supports(const SerializedTargetSet& targets);

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
