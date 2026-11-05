#pragma once

#include "Core/EditorTaskProgressService.h"
#include "Assets/ImportProgressTracker.h"

namespace NLS::Editor::Core
{
// Scoped subscription: ImportProgressTracker callbacks keep a weak reference,
// and retained task history never owns a raw cancellation-token pointer.
class EditorImportTaskProgress
{
public:
    EditorImportTaskProgress(EditorTaskProgressService& tasks, Assets::ImportProgressTracker& imports);
    ~EditorImportTaskProgress();
    EditorTaskHandle FindTask(Assets::ImportJobId job) const;

private:
    struct State;
    std::shared_ptr<State> m_state;
};
}
