#pragma once

#include "Inspector/InspectorTypes.h"
#include <map>

namespace NLS::Editor::Undo { class EditorUndoService; }

namespace NLS::Editor::Inspector
{
// Inspector controls and asset workbenches share one draft and Undo timeline.
// No live resource or Inspector selection pointer is retained by edit commands.
struct ImporterEditingState
{
    std::map<std::string,std::string> settings;
    bool available = false, dirty = false, reimporting = false;
    uint64_t revision = 0;
    std::string error;
};

ImporterEditingState ReadImporterEditingState(const InspectorTargetHandle& target);
bool EditImporterSettings(Undo::EditorUndoService& undo, const InspectorTargetHandle& target,
    const std::map<std::string,std::string>& expected,
    std::map<std::string,std::string> replacement, std::string label,
    std::string mergeKey, std::string& error);
// Uses the existing asynchronous importer Apply/Revert implementation, including
// filesystem conflict checks, cancellation ownership and failed-import rollback.
bool ApplyImporterEditingState(const InspectorTargetHandle& target, std::string& error);
bool RevertImporterEditingState(const InspectorTargetHandle& target, std::string& error);
}
