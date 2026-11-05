#include "Inspector/EditorSelectionService.h"

#include <algorithm>

namespace NLS::Editor::Inspector
{
InspectorTargetSet ComposeSelection(
    const InspectorTargetSet& before, InspectorTargetSet candidates,
    const SelectionOperation operation, const bool preserveActive)
{
    candidates.Normalize();
    InspectorTargetSet result = operation == SelectionOperation::Replace ? candidates : before;
    if (operation != SelectionOperation::Replace)
        for (const auto& target : candidates.targets)
        {
            const bool selected = result.Contains(target);
            if (operation == SelectionOperation::Remove || (operation == SelectionOperation::Toggle && selected))
                std::erase(result.targets, target);
            else if (!selected)
                result.targets.push_back(target);
        }
    if (preserveActive && before.Active() && result.Contains(*before.Active()))
        result.activeTarget = *before.Active();
    else if (operation != SelectionOperation::Remove && candidates.Active() && result.Contains(*candidates.Active()))
        result.activeTarget = *candidates.Active();
    result.Normalize();
    return result;
}

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
    m_notificationPending = true;
    Publish();
}

void EditorSelectionService::BeginUpdate() { ++m_updateDepth; }
void EditorSelectionService::EndUpdate()
{
    if (m_updateDepth) --m_updateDepth;
    Publish();
}
void EditorSelectionService::Publish()
{
    if (m_updateDepth || m_publishing) return;
    m_publishing = true;
    struct ResetPublishing { bool& value; ~ResetPublishing() { value = false; } } reset{m_publishing};
    while (m_notificationPending)
    {
        m_notificationPending = false;
        const auto snapshot = m_selection;
        SelectionChangedEvent.Invoke(snapshot);
    }
}

void EditorSelectionService::Select(const InspectorTargetHandle& target)
{
    InspectorTargetSet targets;
    targets.targets.push_back(target);
    SetSelection(std::move(targets));
}

void EditorSelectionService::ApplySelection(InspectorTargetSet targets, const SelectionOperation operation)
{
    SetSelection(ComposeSelection(m_selection, std::move(targets), operation));
}

void EditorSelectionService::SetActive(const InspectorTargetHandle& target)
{
    if (!m_selection.Contains(target)) return;
    auto selection = m_selection;
    selection.activeTarget = target;
    SetSelection(std::move(selection));
}

bool EditorSelectionService::Contains(const InspectorTargetHandle& target) const
{
    return m_selection.Contains(target);
}

void EditorSelectionService::Clear()
{
    SetSelection({});
}
} // namespace NLS::Editor::Inspector
