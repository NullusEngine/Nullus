#pragma once

#include <Eventing/Event.h>

#include "Inspector/InspectorTypes.h"

namespace NLS::Editor::Inspector
{
enum class SelectionOperation { Replace, Add, Remove, Toggle };

// Pure composition also lets range/marquee gestures recompute from their
// original snapshot instead of accumulating changes on every mouse move.
[[nodiscard]] InspectorTargetSet ComposeSelection(
    const InspectorTargetSet& before, InspectorTargetSet candidates,
    SelectionOperation operation, bool preserveActive = false);

/**
 * The sole editor selection authority.  Views subscribe to this service;
 * selection producers never reach into a concrete panel.
 */
class EditorSelectionService final
{
public:
    static EditorSelectionService& Get();

    [[nodiscard]] const InspectorTargetSet& GetSelection() const;
    void SetSelection(InspectorTargetSet targets);
    void Select(const InspectorTargetHandle& target);
    void ApplySelection(InspectorTargetSet targets, SelectionOperation operation);
    void SetActive(const InspectorTargetHandle& target);
    [[nodiscard]] bool Contains(const InspectorTargetHandle& target) const;
    void BeginUpdate();
    void EndUpdate();
    void Clear();

    Event<const InspectorTargetSet&> SelectionChangedEvent;

private:
    InspectorTargetSet m_selection;
    uint64_t m_nextRevision = 1u;
    size_t m_updateDepth = 0;
    bool m_notificationPending = false, m_publishing = false;
    void Publish();
};

class EditorSelectionUpdate final
{
public:
    EditorSelectionUpdate() { EditorSelectionService::Get().BeginUpdate(); }
    ~EditorSelectionUpdate() { EditorSelectionService::Get().EndUpdate(); }
    EditorSelectionUpdate(const EditorSelectionUpdate&) = delete;
    EditorSelectionUpdate& operator=(const EditorSelectionUpdate&) = delete;
};
} // namespace NLS::Editor::Inspector
