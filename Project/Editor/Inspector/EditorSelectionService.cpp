#include "Inspector/EditorSelectionService.h"

namespace NLS::Editor::Inspector
{
EditorSelectionService& EditorSelectionService::Get()
{
    static EditorSelectionService service;
    return service;
}

const InspectorTargetSet& EditorSelectionService::GetSelection() const
{
    return m_selection;
}

void EditorSelectionService::SetSelection(InspectorTargetSet targets)
{
    targets.Normalize();
    if (targets == m_selection)
        return;

    targets.revision = m_nextRevision++;
    m_selection = std::move(targets);
    SelectionChangedEvent.Invoke(m_selection);
}

void EditorSelectionService::Select(const InspectorTargetHandle& target)
{
    InspectorTargetSet targets;
    targets.targets.push_back(target);
    SetSelection(std::move(targets));
}

void EditorSelectionService::Clear()
{
    SetSelection({});
}
} // namespace NLS::Editor::Inspector
