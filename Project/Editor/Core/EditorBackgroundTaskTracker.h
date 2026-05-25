#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

#include <Jobs/JobTypes.h>

namespace NLS::Editor::Core
{
    class EditorBackgroundTaskTracker
    {
    public:
        explicit EditorBackgroundTaskTracker(size_t capacity);
        ~EditorBackgroundTaskTracker();

        EditorBackgroundTaskTracker(const EditorBackgroundTaskTracker&) = delete;
        EditorBackgroundTaskTracker& operator=(const EditorBackgroundTaskTracker&) = delete;

        bool Track(std::function<void()> task);
        void StopAndComplete();

    private:
        void PruneCompletedLocked();

        const size_t m_capacity = 0u;
        std::vector<NLS::Base::Jobs::JobHandle> m_trackedTasks;
        std::mutex m_mutex;
        bool m_acceptingTasks = true;
    };
}
