#pragma once

#include <functional>
#include <memory>

#include "Serialization/SerializedObject.h"

namespace NLS::Render::Resources
{
class Material;
}

namespace NLS::Editor::Serialization
{
// Canonical byte-stream backend for live Material assets. Property Apply
// updates the in-memory material immediately; Save/Reload remain explicit
// asset actions owned by AssetTargetAdapter.
class MaterialSerializedBackend final : public ISerializedTargetBackend
{
public:
    [[nodiscard]] static std::shared_ptr<MaterialSerializedBackend> Create();
    [[nodiscard]] static bool Supports(const SerializedTargetSet& targets);

#if defined(NLS_ENABLE_TEST_HOOKS)
    // Unit tests exercise the production stream backend with stack-owned
    // Materials, without constructing the full Editor Core::Context.
    using TargetResolverForTesting = std::function<NLS::Render::Resources::Material*(
        const SerializedTargetHandle&)>;
    static void SetTargetResolverForTesting(TargetResolverForTesting resolver);
#endif

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
