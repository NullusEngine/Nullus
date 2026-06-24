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
        EditorBackgroundTaskTracker(size_t softCapacity, size_t hardCapacity);
        ~EditorBackgroundTaskTracker();

        EditorBackgroundTaskTracker(const EditorBackgroundTaskTracker&) = delete;
        EditorBackgroundTaskTracker& operator=(const EditorBackgroundTaskTracker&) = delete;

        bool Track(std::function<void()> task);
        bool TrackOpportunistic(std::function<void()> task);
        void StopAndComplete();

    private:
        bool Track(std::function<void()> task, bool warnWhenCapacityReached);
        void PruneCompletedLocked();

        const size_t m_capacity = 0u;
        const size_t m_hardCapacity = 0u;
        std::vector<NLS::Base::Jobs::JobHandle> m_trackedTasks;
        std::mutex m_mutex;
        bool m_acceptingTasks = true;
    };
}
