#pragma once

#include <Eventing/Event.h>

#include "Inspector/InspectorTypes.h"

namespace NLS::Editor::Inspector
{
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
    void Clear();

    Event<const InspectorTargetSet&> SelectionChangedEvent;

private:
    InspectorTargetSet m_selection;
    uint64_t m_nextRevision = 1u;
};
} // namespace NLS::Editor::Inspector
