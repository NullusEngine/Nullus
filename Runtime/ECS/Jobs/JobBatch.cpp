#include "JobBatch.h"
#include <Base/Jobs/JobSystem.h>
#include <Base/Jobs/JobSafety.h>
#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace NLS::ECS::Detail
{
namespace Jobs = Base::Jobs;
struct JobBatch::Impl
{
    struct Work
    {
        std::function<void(size_t)> function;
        std::atomic<size_t> finished{0};
        size_t expected = 0;
        std::mutex mutex;
        std::exception_ptr error;
        void Execute(size_t index)
        {
            try { function(index); }
            catch (...)
            {
                { std::lock_guard lock(mutex); if (!error) error = std::current_exception(); }
                finished.fetch_add(1, std::memory_order_release);
                throw;
            }
            finished.fetch_add(1, std::memory_order_release);
        }
    };
    struct SingleJob : Jobs::IJob
    {
        std::shared_ptr<Work> work;
        void Execute() { work->Execute(0); }
    };
    struct ParallelJob : Jobs::IJobParallelFor
    {
        std::shared_ptr<Work> work;
        void Execute(uint32_t index) { work->Execute(index); }
    };
    struct Pending { Jobs::JobHandle handle; std::shared_ptr<Work> work; };
    std::vector<Pending> pending;
    size_t jobCount = 0;
};
namespace
{
void CheckScheduling()
{
    if (!Jobs::IsJobSystemInitialized()) throw std::logic_error("Initialize the shared JobSystem before scheduling ECS jobs");
    // Reject before submitting anything: returning early from a forbidden wait
    // would otherwise leave callbacks accessing borrowed storage.
    if (Jobs::IsJobSyncWaitDisallowedForCurrentThread()) throw std::logic_error("Joined ECS jobs require a scope that permits JobSystem completion");
}
}
JobBatch::JobBatch() : m_impl(std::make_unique<Impl>()) {}
JobBatch::~JobBatch()
{
    try { Wait(); } catch (...) {} // Preserve an exception already unwinding.
}
void JobBatch::Add(std::function<void()> function, const char* debugName)
{
    CheckScheduling();
    auto work = std::make_shared<Impl::Work>();
    work->function = [function = std::move(function)](size_t) { function(); }; work->expected = 1;
    m_impl->pending.push_back({{}, work}); // Allocate before scheduling.
    auto& pending = m_impl->pending.back();
    Jobs::JobScheduleOptions options; options.debugName = debugName;
    pending.handle = Jobs::Schedule(Impl::SingleJob{{}, std::move(work)}, options);
    if (!pending.handle.id) { m_impl->pending.pop_back(); throw std::runtime_error("JobSystem rejected an ECS system job"); }
    ++m_impl->jobCount;
}
void JobBatch::AddParallelFor(size_t count, size_t grain, std::function<void(size_t)> function, const char* debugName)
{
    if (!count) return;
    CheckScheduling();
    constexpr auto limit = static_cast<size_t>(std::numeric_limits<int>::max());
    if (count > limit || !grain || grain > limit) throw std::length_error("ECS parallel range exceeds JobSystem limits");
    auto work = std::make_shared<Impl::Work>(); work->function = std::move(function); work->expected = count;
    m_impl->pending.push_back({{}, work});
    auto& pending = m_impl->pending.back();
    Jobs::JobParallelForScheduleOptions options; options.batchSize = static_cast<uint32_t>(grain); options.debugName = debugName;
    const auto lanes = std::max(1u, Jobs::GetJobWorkerCount() + 1u);
    const auto shards = Jobs::CalculateJobCountWithMinIndicesPerJob(static_cast<int>(count), static_cast<int>(grain), static_cast<int>(lanes));
    pending.handle = Jobs::ScheduleParallelFor(Impl::ParallelJob{{}, std::move(work)}, static_cast<uint32_t>(count), options);
    if (!pending.handle.id) { m_impl->pending.pop_back(); throw std::runtime_error("JobSystem rejected an ECS parallel range"); }
    m_impl->jobCount += static_cast<size_t>(shards);
}
void JobBatch::Wait()
{
    std::exception_ptr error;
    for (auto& pending : m_impl->pending)
    {
        Jobs::Complete(pending.handle);
        auto& work = *pending.work;
        std::lock_guard lock(work.mutex);
        if (!error) error = work.error;
        if (!error && work.finished.load(std::memory_order_acquire) != work.expected)
            error = std::make_exception_ptr(std::runtime_error("ECS job failed or was cancelled before its range completed"));
    }
    m_impl->pending.clear();
    if (error) std::rethrow_exception(error);
}
size_t JobBatch::GetJobCount() const { return m_impl->jobCount; }
}
