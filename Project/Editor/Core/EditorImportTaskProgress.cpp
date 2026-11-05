#include "Core/EditorImportTaskProgress.h"

#include <map>
#include <mutex>
#include <cmath>

namespace NLS::Editor::Core
{
struct EditorImportTaskProgress::State
{
    State(EditorTaskProgressService& service, Assets::ImportProgressTracker& tracker)
        : tasks(service), imports(tracker) {}
    EditorTaskProgressService& tasks;
    Assets::ImportProgressTracker& imports;
    std::mutex mutex;
    std::map<uint64_t, EditorTaskHandle> active;
    bool closed = false;
};

EditorImportTaskProgress::EditorImportTaskProgress(
    EditorTaskProgressService& tasks, Assets::ImportProgressTracker& imports)
    : m_state(std::make_shared<State>(tasks, imports))
{
    imports.Subscribe([weak = std::weak_ptr<State>(m_state)](const Assets::ImportProgressEvent& event)
    {
        const auto state = weak.lock();
        if (!state) return;
        std::lock_guard lock(state->mutex);
        if (state->closed) return;
        auto found = state->active.find(event.jobId.value);
        if (found == state->active.end())
        {
            if (event.terminalStatus != Assets::ImportJobTerminalStatus::None) return;
            // Subscribers run outside the import mutex. A delayed progress
            // callback can arrive after FinishJob; never resurrect that job.
            const auto current = state->imports.GetCurrentEvent(event.jobId);
            if (!current || current->terminalStatus != Assets::ImportJobTerminalStatus::None) return;
            EditorTaskProgressService::CancelHandler cancel;
            if (Assets::IsImportCancellable(event))
                cancel = [weak, job = event.jobId]
                {
                    if (const auto state = weak.lock())
                        if (const auto token = state->imports.GetCancellationToken(job)) token->get().Cancel();
                };
            found = state->active.emplace(event.jobId.value,
                state->tasks.BeginTask("Import assets", event.sourcePath, 0.0f, std::move(cancel))).first;
        }
        std::string detail = event.sourcePath + " [" + event.targetPlatform + "] - " + event.message;
        detail += " (batch " + std::to_string(static_cast<int>(std::round(event.normalizedProgress * 100.0))) + "%)";
        if (event.terminalStatus == Assets::ImportJobTerminalStatus::None)
        {
            found->second.Update(std::move(detail), static_cast<float>(event.localProgress));
            if (event.cancellationRequested) state->tasks.RequestCancel(found->second.Id());
        }
        else
        {
            const auto terminal = event.terminalStatus == Assets::ImportJobTerminalStatus::Succeeded
                ? EditorTaskState::Succeeded : event.terminalStatus == Assets::ImportJobTerminalStatus::Cancelled
                ? EditorTaskState::Cancelled : EditorTaskState::Failed;
            found->second.Finish(terminal, std::move(detail));
            state->active.erase(found);
        }
    });
}

EditorImportTaskProgress::~EditorImportTaskProgress()
{
    std::lock_guard lock(m_state->mutex);
    m_state->closed = true;
    for (const auto& [id, task] : m_state->active)
        task.Finish(EditorTaskState::Failed, "Import tracking ended before completion");
}

EditorTaskHandle EditorImportTaskProgress::FindTask(Assets::ImportJobId job) const
{
    std::lock_guard lock(m_state->mutex);
    const auto found = m_state->active.find(job.value);
    return found == m_state->active.end() ? EditorTaskHandle{} : found->second;
}
}
