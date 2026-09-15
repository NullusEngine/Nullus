#pragma once

#include <string>
#include <vector>

#include "Inspector/InspectorTypes.h"
#include "Undo/EditorUndoService.h"

namespace NLS::Editor::Inspector
{
// Inspector-owned commands share the editor-wide history with Hierarchy,
// Scene View and asset workflows. These helpers only describe durable scope
// and affected identities; property value storage belongs to SerializedObject
// byte slices and never to PropertyNode callbacks.
[[nodiscard]] Undo::UndoScopeId MakeInspectorUndoScope(const InspectorTargetHandle& target);
[[nodiscard]] Undo::UndoScopeId MakeInspectorAssetUndoScope(
    const std::string& resourcePath,
    const std::string& assetId = {});
[[nodiscard]] Undo::UndoCommandInfo BuildInspectorUndoCommandInfo(
    std::string label,
    const std::vector<InspectorTargetHandle>& targets,
    const PropertyPath& path = {},
    bool allowMerge = false);
} // namespace NLS::Editor::Inspector
