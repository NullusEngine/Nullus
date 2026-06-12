#pragma once

#include <cstdint>
#include <mutex>

#include "RenderDef.h"

struct ID3D12CommandQueue;

namespace NLS::Render::RHI::DX12
{
    namespace Detail
    {
        NLS_RENDER_API std::mutex* ResolveQueueMutex(ID3D12CommandQueue* queue);
        NLS_RENDER_API void ReleaseQueueMutex(ID3D12CommandQueue* queue);
        NLS_RENDER_API std::uint64_t ReserveQueueProfilerSubmitSequence(ID3D12CommandQueue* queue);
        NLS_RENDER_API void WaitForQueueProfilerSubmitSequence(
            ID3D12CommandQueue* queue,
            std::uint64_t sequence);
        NLS_RENDER_API void CompleteQueueProfilerSubmitSequence(
            ID3D12CommandQueue* queue,
            std::uint64_t sequence);
    }

    class ScopedDX12QueueLock final
    {
    public:
        explicit ScopedDX12QueueLock(ID3D12CommandQueue* queue)
            : m_mutex(Detail::ResolveQueueMutex(queue))
            , m_lock(m_mutex != nullptr
                ? std::unique_lock<std::mutex>(*m_mutex)
                : std::unique_lock<std::mutex>{})
        {
        }

        ScopedDX12QueueLock(const ScopedDX12QueueLock&) = delete;
        ScopedDX12QueueLock& operator=(const ScopedDX12QueueLock&) = delete;

    private:
        std::mutex* m_mutex = nullptr;
        std::unique_lock<std::mutex> m_lock;
    };

    class ScopedDX12QueueProfilerSubmissionOrder final
    {
    public:
        ScopedDX12QueueProfilerSubmissionOrder(
            ID3D12CommandQueue* queue,
            const std::uint64_t sequence)
            : m_queue(queue)
            , m_sequence(sequence)
        {
            if (m_sequence != 0u)
                Detail::WaitForQueueProfilerSubmitSequence(m_queue, m_sequence);
        }

        ~ScopedDX12QueueProfilerSubmissionOrder()
        {
            if (m_sequence != 0u)
                Detail::CompleteQueueProfilerSubmitSequence(m_queue, m_sequence);
        }

        ScopedDX12QueueProfilerSubmissionOrder(const ScopedDX12QueueProfilerSubmissionOrder&) = delete;
        ScopedDX12QueueProfilerSubmissionOrder& operator=(const ScopedDX12QueueProfilerSubmissionOrder&) = delete;

    private:
        ID3D12CommandQueue* m_queue = nullptr;
        std::uint64_t m_sequence = 0u;
    };
}
