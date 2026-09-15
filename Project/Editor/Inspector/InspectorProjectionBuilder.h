#pragma once

#include <string_view>
#include <vector>

#include "Inspector/InspectorTypes.h"

namespace NLS::Editor::Serialization
{
class SerializedObject;
}

namespace NLS::Editor::Inspector
{
// Builds the Inspector's disposable display projection from canonical
// SerializedProperty cursors. PropertyNode intentionally receives no binding,
// target pointer, or write callback: all reads and mutations continue through
// the owning SerializedObject.
class InspectorProjectionBuilder final
{
public:
    // Builds a hierarchy that mirrors the visible SerializedProperty cursor
    // tree. PropertyNode remains presentation-only; the nodes hold paths and
    // display state, while every value is still resolved from SerializedObject.
    [[nodiscard]] static std::vector<PropertyNode> BuildPropertyProjection(
        Serialization::SerializedObject& serializedObject,
        const InspectorTargetSet& targets,
        std::string_view searchText);
};
} // namespace NLS::Editor::Inspector
