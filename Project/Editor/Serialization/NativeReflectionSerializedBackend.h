#pragma once

#include <memory>

#include "Serialization/SerializedObject.h"

namespace NLS::Editor::Serialization
{
// Canonical native-object backend for SerializedObject. It consumes stable
// targets and reflected fields directly, never PropertyNode callbacks.
class NativeReflectionSerializedBackend final : public ISerializedTargetBackend
{
public:
    [[nodiscard]] static std::shared_ptr<NativeReflectionSerializedBackend> Create();

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

    [[nodiscard]] static Object* Resolve(const SerializedTargetHandle& target);
};
} // namespace NLS::Editor::Serialization
