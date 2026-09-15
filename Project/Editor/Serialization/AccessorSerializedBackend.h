#pragma once

#include <memory>

#include "Serialization/SerializedObject.h"

namespace NLS::Editor::Serialization
{
class AccessorSerializedBackend final : public ISerializedTargetBackend
{
public:
    [[nodiscard]] static std::shared_ptr<AccessorSerializedBackend> Create();
    // A deliberately narrow stream for component chrome (`enabled`). It keeps
    // the component card header on the canonical SerializedObject path without
    // exposing that synthetic field to a component editor's body iterator.
    [[nodiscard]] static std::shared_ptr<AccessorSerializedBackend> CreateComponentHeader();
    [[nodiscard]] static bool Supports(const SerializedTargetSet& targets);
    [[nodiscard]] static bool SupportsComponentHeader(const SerializedTargetSet& targets);

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

private:
    explicit AccessorSerializedBackend(bool componentHeaderOnly)
        : m_componentHeaderOnly(componentHeaderOnly)
    {
    }

    bool m_componentHeaderOnly = false;
};
} // namespace NLS::Editor::Serialization
