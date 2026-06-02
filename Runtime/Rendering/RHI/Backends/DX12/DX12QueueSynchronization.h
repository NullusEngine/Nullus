#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>

#include "RenderDef.h"

struct ID3D12CommandQueue;

namespace NLS::Render::RHI::DX12
{
    namespace Detail
    {
        inline std::mutex& QueueMutexRegistryLock()
        {
            static std::mutex mutex;
            return mutex;
        }

        inline std::unordered_map<ID3D12CommandQueue*, std::unique_ptr<std::mutex>>& QueueMutexRegistry()
        {
            static std::unordered_map<ID3D12CommandQueue*, std::unique_ptr<std::mutex>> registry;
            return registry;
        }

        inline std::mutex* ResolveQueueMutex(ID3D12CommandQueue* queue)
        {
            if (queue == nullptr)
                return nullptr;

            std::lock_guard<std::mutex> lock(QueueMutexRegistryLock());
            auto& mutex = QueueMutexRegistry()[queue];
            if (mutex == nullptr)
                mutex = std::make_unique<std::mutex>();
            return mutex.get();
        }

        inline void ReleaseQueueMutex(ID3D12CommandQueue* queue)
        {
            if (queue == nullptr)
                return;

            std::lock_guard<std::mutex> lock(QueueMutexRegistryLock());
            QueueMutexRegistry().erase(queue);
        }
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
}
