#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace NLS::Editor::Assets
{
struct EditorAssetDragPayload;
}

namespace NLS::Editor::Inspector
{
// Immediate-mode picker state belongs to the owning InspectorView, never to an
// edited target. Keeping only the search text here means the picker does not
// retain object pointers or participate in Undo persistence.
struct InspectorObjectReferencePickerState
{
    std::unordered_map<std::string, std::string> searchTextByKey;
};

struct InspectorObjectReferencePickerItem
{
    std::string displayName;
    // When empty, displayName is used for matching.
    std::string searchText;
    // The callback must perform its mutation through the caller's edit session.
    std::function<bool()> select;
};

struct InspectorObjectReferencePickerConfig
{
    // Both values are scoped through ImGui's ID stack. searchKey survives popup
    // close/reopen so the same field keeps its query while the Inspector lives.
    std::string id;
    std::string searchKey;
    std::string displayName;
    std::string displayTooltip;
    std::string noMatchesLabel = "No matching references";
    float uiScale = 1.0f;

    // Asset drag/drop is optional: scene-only reference fields simply leave
    // these callbacks empty while still using the same picker UI.
    std::function<bool(const Assets::EditorAssetDragPayload&)> acceptsAsset;
    std::function<bool(const Assets::EditorAssetDragPayload&)> assignAsset;

    // Physical file drag fallback. Asset Browser falls back to a "File"
    // payload when an asset has not yet produced an editor asset identity;
    // typed fields that can accept those paths opt in here. Both callbacks
    // must be set for the fallback to be active.
    std::function<bool(const std::string& path)> acceptsFile;
    std::function<bool(const std::string& path)> assignFile;
    std::function<void()> ping;
    std::function<bool()> clear;
    std::function<std::vector<InspectorObjectReferencePickerItem>()> buildItems;
    std::function<void()> open;
};

// Draws the engine-style object field shared by Inspector custom editors and
// generic reflected properties: current value, typed search popup, compatible
// asset drop, picker button, Ping and Clear. Selection callbacks are invoked
// synchronously and return true only after their transactional edit succeeded.
bool DrawInspectorObjectReferencePicker(
    InspectorObjectReferencePickerState& state,
    const InspectorObjectReferencePickerConfig& config);
} // namespace NLS::Editor::Inspector
