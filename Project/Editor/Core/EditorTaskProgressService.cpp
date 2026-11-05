#include "Core/EditorTaskProgressService.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <utility>
#include <ServiceLocator.h>

namespace NLS::Editor::Core
{
struct EditorTaskProgressState
{
    struct Record
    {
        EditorTaskSnapshot snapshot;
        EditorTaskProgressService::CancelHandler cancel;
    };
    mutable std::mutex mutex;
    EditorTaskId nextId = 1;
    std::map<EditorTaskId, Record> tasks;
    std::deque<EditorTaskId> completed;
};

namespace
{
std::optional<float> Normalize(std::optional<float> progress)
{
    if (!progress || !std::isfinite(*progress)) return std::nullopt;
    return std::clamp(*progress, 0.0f, 1.0f);
}

bool UpdateTaskState(const std::shared_ptr<EditorTaskProgressState>& state, EditorTaskId id,
    std::string detail, std::optional<float> progress)
{
    std::lock_guard lock(state->mutex);
    const auto found = state->tasks.find(id);
    if (found == state->tasks.end() || found->second.snapshot.state != EditorTaskState::Running) return false;
    auto& task = found->second.snapshot;
    task.detail = std::move(detail);
    progress = Normalize(progress);
    if (progress && task.progress) progress = std::max(*progress, *task.progress);
    task.progress = progress;
    return true;
}

bool FinishTaskState(const std::shared_ptr<EditorTaskProgressState>& state, EditorTaskId id,
    EditorTaskState terminal, std::string detail)
{
    if (terminal == EditorTaskState::Running) return false;
    // Destroy callbacks after releasing the mutex: their captures may own other
    // reporting handles and run destructors that report completion themselves.
    EditorTaskProgressService::CancelHandler retiredCancel;
    {
        std::lock_guard lock(state->mutex);
        const auto found = state->tasks.find(id);
        if (found == state->tasks.end() || found->second.snapshot.state != EditorTaskState::Running) return false;
        auto& task = found->second.snapshot;
        task.state = terminal;
        if (!detail.empty()) task.detail = std::move(detail);
        if (terminal == EditorTaskState::Succeeded) task.progress = 1.0f;
        task.cancellable = false;
        retiredCancel = std::move(found->second.cancel);
        state->completed.push_back(id);
        while (state->completed.size() > EditorTaskProgressService::CompletedHistoryLimit)
        {
            state->tasks.erase(state->completed.front());
            state->completed.pop_front();
        }
    }
    return true;
}
}

struct EditorTaskLease
{
    std::shared_ptr<EditorTaskProgressState> state;
    EditorTaskId id = 0;
    ~EditorTaskLease()
    {
        FinishTaskState(state, id, EditorTaskState::Failed, "Task ended before completion");
    }
};

EditorTaskId EditorTaskHandle::Id() const { return m_lease ? m_lease->id : 0; }
bool EditorTaskHandle::IsRunning() const
{
    if (!m_lease) return false;
    std::lock_guard lock(m_lease->state->mutex);
    const auto found = m_lease->state->tasks.find(Id());
    return found != m_lease->state->tasks.end() && found->second.snapshot.state == EditorTaskState::Running;
}
bool EditorTaskHandle::Update(std::string detail, std::optional<float> progress) const
{
    return m_lease && UpdateTaskState(m_lease->state, Id(), std::move(detail), progress);
}
bool EditorTaskHandle::Finish(EditorTaskState terminal, std::string detail) const
{
    return m_lease && FinishTaskState(m_lease->state, Id(), terminal, std::move(detail));
}

EditorTaskProgressService::EditorTaskProgressService() : m_state(std::make_shared<EditorTaskProgressState>()) {}
EditorTaskProgressService::~EditorTaskProgressService()
{
    if (NLS::Core::ServiceLocator::Contains<EditorTaskProgressService>() &&
        &NLS_SERVICE(EditorTaskProgressService) == this)
        NLS::Core::ServiceLocator::Remove<EditorTaskProgressService>();
}

EditorTaskHandle EditorTaskProgressService::BeginTask(std::string title, std::string detail,
    std::optional<float> progress, CancelHandler cancel)
{
    EditorTaskHandle handle;
    handle.m_lease = std::make_shared<EditorTaskLease>();
    handle.m_lease->state = m_state;
    std::lock_guard lock(m_state->mutex);
    const auto id = m_state->nextId++;
    handle.m_lease->id = id;
    m_state->tasks.emplace(id, EditorTaskProgressState::Record{
        {id, std::move(title), std::move(detail), Normalize(progress), EditorTaskState::Running, bool(cancel), false},
        std::move(cancel)});
    return handle;
}

bool EditorTaskProgressService::UpdateTask(EditorTaskId id, std::string detail, std::optional<float> progress)
{
    return UpdateTaskState(m_state, id, std::move(detail), progress);
}
bool EditorTaskProgressService::FinishTask(EditorTaskId id, EditorTaskState terminal, std::string detail)
{
    return FinishTaskState(m_state, id, terminal, std::move(detail));
}

bool EditorTaskProgressService::RequestCancel(EditorTaskId id)
{
    CancelHandler cancel;
    {
        std::lock_guard lock(m_state->mutex);
        const auto found = m_state->tasks.find(id);
        if (found == m_state->tasks.end()) return false;
        auto& task = found->second.snapshot;
        if (task.state != EditorTaskState::Running || !task.cancellable || task.cancellationRequested) return false;
        task.cancellationRequested = true;
        task.cancellable = false;
        cancel = found->second.cancel;
    }
    try { cancel(); }
    catch (const std::exception& error) { FinishTask(id, EditorTaskState::Failed, error.what()); }
    catch (...) { FinishTask(id, EditorTaskState::Failed, "Task cancellation failed"); }
    return true;
}

std::vector<EditorTaskSnapshot> EditorTaskProgressService::GetSnapshot() const
{
    std::lock_guard lock(m_state->mutex);
    std::vector<EditorTaskSnapshot> result;
    for (const auto& [id, record] : m_state->tasks)
        if (record.snapshot.state == EditorTaskState::Running) result.push_back(record.snapshot);
    for (auto it = m_state->completed.rbegin(); it != m_state->completed.rend(); ++it)
        result.push_back(m_state->tasks.at(*it).snapshot);
    return result;
}

void EditorTaskProgressService::ClearCompleted()
{
    std::lock_guard lock(m_state->mutex);
    for (const auto id : m_state->completed) m_state->tasks.erase(id);
    m_state->completed.clear();
}

EditorTaskHandle TryBeginEditorTask(std::string title, std::string detail, std::optional<float> progress,
    EditorTaskProgressService::CancelHandler cancel)
{
    if (!NLS::Core::ServiceLocator::Contains<EditorTaskProgressService>()) return {};
    return NLS_SERVICE(EditorTaskProgressService).BeginTask(std::move(title), std::move(detail), progress, std::move(cancel));
}

const char* EditorTaskStateName(EditorTaskState state)
{
    switch (state)
    {
    case EditorTaskState::Running: return "Running";
    case EditorTaskState::Succeeded: return "Succeeded";
    case EditorTaskState::Failed: return "Failed";
    case EditorTaskState::Cancelled: return "Cancelled";
    }
    return "Unknown";
}
}
