#pragma once

#include <string>

#include "Inspector/InspectorTypes.h"

namespace NLS::Scripting
{
class ScriptComponent;
}

namespace NLS::Editor::Inspector
{
// Script-specific presentation sidecar. Serialization and all value edits are
// owned by the canonical byte-stream SerializedObject; this utility only
// supplies display metadata for its PropertyNode projection.
class ScriptSerializedAdapter final
{
public:
    // Used by the GameObject adapter and direct script targets so a shared
    // native ScriptComponent is presented as its concrete C#/Lua asset.
    [[nodiscard]] static std::string GetComponentDisplayName(const NLS::Scripting::ScriptComponent& component);
    [[nodiscard]] static uint64_t GetComponentStructureVersion(const NLS::Scripting::ScriptComponent& component);
    static void ApplyPropertyPresentation(
        const NLS::Scripting::ScriptComponent& component,
        PropertyNode& node);
};
} // namespace NLS::Editor::Inspector
