#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Editor::Core
{
enum class EditorTaskState { Running, Succeeded, Failed, Cancelled };
using EditorTaskId = uint64_t;

struct EditorTaskSnapshot
{
    EditorTaskId id = 0;
    std::string title;
    std::string detail;
    std::optional<float> progress;
    EditorTaskState state = EditorTaskState::Running;
    bool cancellable = false;
    bool cancellationRequested = false;
};

struct EditorTaskProgressState;
struct EditorTaskLease;

// Copies share one operation. Dropping the last unfinished handle records a
// failure, including worker exceptions and rejected/abandoned queued work.
class EditorTaskHandle
{
public:
    EditorTaskHandle() = default;
    EditorTaskId Id() const;
    bool IsRunning() const;
    explicit operator bool() const { return Id() != 0; }
    bool Update(std::string detail, std::optional<float> progress = std::nullopt) const;
    bool Finish(EditorTaskState state, std::string detail = {}) const;

private:
    friend class EditorTaskProgressService;
    std::shared_ptr<EditorTaskLease> m_lease;
};

// Progress reporting only: work stays on its existing scheduler. Handles retain
// the state, so retiring a preview/Context cannot leave a worker with a UI pointer.
class EditorTaskProgressService
{
public:
    using CancelHandler = std::function<void()>;
    static constexpr size_t CompletedHistoryLimit = 32;

    EditorTaskProgressService();
    ~EditorTaskProgressService();
    EditorTaskProgressService(const EditorTaskProgressService&) = delete;
    EditorTaskProgressService& operator=(const EditorTaskProgressService&) = delete;

    EditorTaskHandle BeginTask(std::string title, std::string detail = {},
        std::optional<float> progress = std::nullopt, CancelHandler cancel = {});
    bool UpdateTask(EditorTaskId id, std::string detail, std::optional<float> progress = std::nullopt);
    bool FinishTask(EditorTaskId id, EditorTaskState state, std::string detail = {});
    bool RequestCancel(EditorTaskId id);
    std::vector<EditorTaskSnapshot> GetSnapshot() const;
    void ClearCompleted();

private:
    std::shared_ptr<EditorTaskProgressState> m_state;
};

// Main-thread producers can opt into the Context's progress service. Standalone
// tools/tests without an Editor keep their existing execution behavior.
EditorTaskHandle TryBeginEditorTask(std::string title, std::string detail = {},
    std::optional<float> progress = std::nullopt, EditorTaskProgressService::CancelHandler cancel = {});
const char* EditorTaskStateName(EditorTaskState state);
}
