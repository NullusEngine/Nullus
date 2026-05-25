#include "Core/EditorBackgroundTaskTracker.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <string>

#include <Debug/Logger.h>
#include <Jobs/BackgroundJobQueue.h>
#include <Jobs/JobSystem.h>

namespace NLS::Editor::Core
{
    namespace
    {
        struct TaskPayload
        {
            std::function<void()> task;
            std::atomic<bool> claimed{false};
        };

        void DeleteTaskPayloadIfUnclaimed(void* userData)
        {
            auto* payload = static_cast<TaskPayload*>(userData);
            if (payload != nullptr && !payload->claimed.exchange(true, std::memory_order_acq_rel))
                delete payload;
        }
    }

    EditorBackgroundTaskTracker::EditorBackgroundTaskTracker(const size_t capacity)
        : m_capacity(capacity)
    {
    }

    EditorBackgroundTaskTracker::~EditorBackgroundTaskTracker()
    {
        StopAndComplete();
    }

    bool EditorBackgroundTaskTracker::Track(std::function<void()> task)
    {
        if (!task)
            return false;

        if (!NLS::Base::Jobs::IsJobSystemInitialized())
        {
            NLS_LOG_WARNING("Editor background task was rejected because the shared JobSystem is not initialized");
            return false;
        }

        auto* payload = new TaskPayload{std::move(task)};
        NLS::Base::Jobs::BackgroundJobDesc desc;
        desc.userData = payload;
        desc.cancelFunction = DeleteTaskPayloadIfUnclaimed;
        desc.cancelUserData = payload;
        desc.debugName = "EditorActions::TrackBackgroundTask";
        desc.function = [](void* userData)
        {
            auto* rawPayload = static_cast<TaskPayload*>(userData);
            if (rawPayload == nullptr ||
                rawPayload->claimed.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }

            std::unique_ptr<TaskPayload> payload(rawPayload);
            try
            {
                payload->task();
            }
            catch (const std::exception& exception)
            {
                NLS_LOG_ERROR(std::string("Editor background task failed: ") + exception.what());
            }
            catch (...)
            {
                NLS_LOG_ERROR("Editor background task failed with an unknown exception");
            }
        };

        std::lock_guard lock(m_mutex);
        if (!m_acceptingTasks)
        {
            DeleteTaskPayloadIfUnclaimed(payload);
            return false;
        }

        PruneCompletedLocked();
        if (m_trackedTasks.size() >= m_capacity)
        {
            DeleteTaskPayloadIfUnclaimed(payload);
            NLS_LOG_WARNING("Editor background task queue is full; dropping queued work");
            return false;
        }

        const auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
        if (handle.id == 0u)
        {
            DeleteTaskPayloadIfUnclaimed(payload);
            NLS_LOG_WARNING("Editor background task was rejected by the shared JobSystem");
            return false;
        }

        m_trackedTasks.push_back(handle);
        return true;
    }

    void EditorBackgroundTaskTracker::StopAndComplete()
    {
        std::vector<NLS::Base::Jobs::JobHandle> trackedTasks;
        {
            std::lock_guard lock(m_mutex);
            if (!m_acceptingTasks && m_trackedTasks.empty())
                return;

            m_acceptingTasks = false;
            trackedTasks.swap(m_trackedTasks);
        }

        for (auto& handle : trackedTasks)
            NLS::Base::Jobs::Complete(handle);
    }

    void EditorBackgroundTaskTracker::PruneCompletedLocked()
    {
        m_trackedTasks.erase(
            std::remove_if(
                m_trackedTasks.begin(),
                m_trackedTasks.end(),
                [](const NLS::Base::Jobs::JobHandle& handle)
                {
                    return NLS::Base::Jobs::IsCompleted(handle);
                }),
            m_trackedTasks.end());
    }
}
