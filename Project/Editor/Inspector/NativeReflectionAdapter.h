#pragma once

#include "Inspector/InspectorTypes.h"

namespace NLS::Editor::Inspector
{
class NativeReflectionAdapter final : public IInspectorTargetAdapter
{
public:
    [[nodiscard]] bool CanBuild(const InspectorTargetSet& targets) const override;
    [[nodiscard]] uint64_t GetStructureVersion(const InspectorTargetSet& targets) const override;
    InspectorPresentation BuildPresentation(const InspectorTargetSet& targets, const InspectorBuildContext& context) const override;

    [[nodiscard]] static Object* ResolveObject(const InspectorTargetHandle& handle);
};
} // namespace NLS::Editor::Inspector
