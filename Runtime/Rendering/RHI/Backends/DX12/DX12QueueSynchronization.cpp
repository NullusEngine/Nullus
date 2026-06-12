#include "Rendering/RHI/Backends/DX12/DX12QueueSynchronization.h"

#include <condition_variable>
#include <memory>
#include <unordered_map>

namespace NLS::Render::RHI::DX12::Detail
{
    namespace
    {
        struct QueueSynchronizationState
        {
            std::mutex queueMutex;
            std::mutex profilerSubmitOrderMutex;
            std::condition_variable profilerSubmitOrderCv;
            std::uint64_t nextProfilerSubmitSequence = 1u;
            std::uint64_t nextProfilerSubmitToPublish = 1u;
        };

        std::mutex& QueueMutexRegistryLock()
        {
            static auto* mutex = new std::mutex();
            return *mutex;
        }

        std::unordered_map<ID3D12CommandQueue*, std::unique_ptr<QueueSynchronizationState>>& QueueMutexRegistry()
        {
            static auto* registry =
                new std::unordered_map<ID3D12CommandQueue*, std::unique_ptr<QueueSynchronizationState>>();
            return *registry;
        }

        QueueSynchronizationState* ResolveQueueSynchronizationState(ID3D12CommandQueue* queue)
        {
            if (queue == nullptr)
                return nullptr;

            std::lock_guard<std::mutex> lock(QueueMutexRegistryLock());
            auto& state = QueueMutexRegistry()[queue];
            if (state == nullptr)
                state = std::make_unique<QueueSynchronizationState>();
            return state.get();
        }
    }

    std::mutex* ResolveQueueMutex(ID3D12CommandQueue* queue)
    {
        auto* state = ResolveQueueSynchronizationState(queue);
        return state != nullptr ? &state->queueMutex : nullptr;
    }

    void ReleaseQueueMutex(ID3D12CommandQueue* queue)
    {
        (void)queue;
        // Queue synchronization state is process-lifetime by design. Profiler and raw DX12 queue
        // users can outlive NativeDX12Queue wrappers during shutdown/reinit, so erasing here would
        // invalidate ScopedDX12QueueLock raw pointers that are still in use on other threads.
    }

    std::uint64_t ReserveQueueProfilerSubmitSequence(ID3D12CommandQueue* queue)
    {
        auto* state = ResolveQueueSynchronizationState(queue);
        if (state == nullptr)
            return 0u;

        std::lock_guard<std::mutex> lock(state->profilerSubmitOrderMutex);
        return state->nextProfilerSubmitSequence++;
    }

    void WaitForQueueProfilerSubmitSequence(
        ID3D12CommandQueue* queue,
        const std::uint64_t sequence)
    {
        auto* state = ResolveQueueSynchronizationState(queue);
        if (state == nullptr || sequence == 0u)
            return;

        std::unique_lock<std::mutex> lock(state->profilerSubmitOrderMutex);
        state->profilerSubmitOrderCv.wait(
            lock,
            [&state, sequence]()
            {
                return state->nextProfilerSubmitToPublish >= sequence;
            });
    }

    void CompleteQueueProfilerSubmitSequence(
        ID3D12CommandQueue* queue,
        const std::uint64_t sequence)
    {
        auto* state = ResolveQueueSynchronizationState(queue);
        if (state == nullptr || sequence == 0u)
            return;

        {
            std::lock_guard<std::mutex> lock(state->profilerSubmitOrderMutex);
            if (state->nextProfilerSubmitToPublish <= sequence)
                state->nextProfilerSubmitToPublish = sequence + 1u;
        }
        state->profilerSubmitOrderCv.notify_all();
    }
}
