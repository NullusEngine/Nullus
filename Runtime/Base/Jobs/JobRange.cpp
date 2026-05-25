#include "Jobs/JobRange.h"

#include <algorithm>
#include <limits>

namespace NLS::Base::Jobs
{
    namespace
    {
        constexpr uint32_t kPackedBatchLimit = 0xffffu - 2u;
        constexpr int kMaxWorkStealingJobCount = 256;

        uint32_t PackStartEnd(const int start, const int end)
        {
            return (static_cast<uint32_t>(end) << 16u) | static_cast<uint32_t>(start);
        }

        int PackedStart(const uint32_t value)
        {
            return static_cast<int>(value & 0xffffu);
        }

        int PackedEnd(const uint32_t value)
        {
            return static_cast<int>(value >> 16u);
        }

        size_t RangeSlot(const WorkStealingRange& range, const int jobIndex, const int phase)
        {
            return static_cast<size_t>(jobIndex * range.phaseCount + phase);
        }
    }

    int CalculateJobCountWithMinIndicesPerJob(
        const int arrayLength,
        const int minimumIndicesPerJob,
        const int workerThreads)
    {
        if (arrayLength <= 0)
            return 0;

        const int safeMinimum = std::max(1, minimumIndicesPerJob);
        const int64_t workLimitedCount64 =
            (static_cast<int64_t>(arrayLength) + safeMinimum - 1) / safeMinimum;
        const int workLimitedCount = static_cast<int>(std::min<int64_t>(
            workLimitedCount64,
            std::numeric_limits<int>::max()));
        const int safeWorkerCount = std::max(1, workerThreads);
        return std::max(1, std::min(workLimitedCount, safeWorkerCount));
    }

    int ConfigureBlockRanges(
        JobBlockRange* blockRanges,
        const int arrayLength,
        const int blockRangeCount)
    {
        if (blockRanges == nullptr || arrayLength <= 0 || blockRangeCount <= 0)
            return 0;

        const int resolvedCount = std::min(arrayLength, blockRangeCount);
        const int baseRangeSize = arrayLength / resolvedCount;
        const int remainder = arrayLength % resolvedCount;

        size_t start = 0u;
        for (int index = 0; index < resolvedCount; ++index)
        {
            const size_t rangeSize = static_cast<size_t>(baseRangeSize + (index < remainder ? 1 : 0));
            blockRanges[index].startIndex = start;
            blockRanges[index].rangeSize = rangeSize;
            blockRanges[index].rangesTotal = static_cast<size_t>(resolvedCount);
            start += rangeSize;
        }

        return resolvedCount;
    }

    int ConfigureBlockRangesWithMinIndicesPerJob(
        JobBlockRange* blockRanges,
        const int arrayLength,
        const int minimumIndicesPerJob,
        const int workerThreads)
    {
        const int jobCount = CalculateJobCountWithMinIndicesPerJob(arrayLength, minimumIndicesPerJob, workerThreads);
        return ConfigureBlockRanges(blockRanges, arrayLength, jobCount);
    }

    void InitializeWorkStealingRange(
        WorkStealingRange& range,
        const int iterationCount,
        const int requestedBatchSize,
        const int requestedJobCount)
    {
        range.startEndByJobAndPhase.clear();
        range.currentPhaseByJob.clear();
        range.batchSize = std::max(1, requestedBatchSize);
        range.totalIterationCount = std::max(0, iterationCount);

        const int64_t totalBatches64 =
            (static_cast<int64_t>(range.totalIterationCount) + range.batchSize - 1) / range.batchSize;
        const int totalBatches = static_cast<int>(std::min<int64_t>(
            totalBatches64,
            std::numeric_limits<int>::max()));
        const int effectiveBatches = std::max(1, totalBatches);
        range.jobCount = std::clamp(
            requestedJobCount,
            1,
            std::min(effectiveBatches, kMaxWorkStealingJobCount));
        range.batchesPerPhase = static_cast<int>(kPackedBatchLimit);
        const int64_t phaseCount64 =
            (static_cast<int64_t>(totalBatches) + range.batchesPerPhase - 1) / range.batchesPerPhase;
        range.phaseCount = std::max(1, static_cast<int>(phaseCount64));

        range.startEndByJobAndPhase =
            std::vector<uint32_t>(static_cast<size_t>(range.jobCount) * static_cast<size_t>(range.phaseCount));
        range.currentPhaseByJob =
            std::vector<int>(static_cast<size_t>(range.jobCount));

        for (int jobIndex = 0; jobIndex < range.jobCount; ++jobIndex)
            range.currentPhaseByJob[static_cast<size_t>(jobIndex)] = 0;

        for (int phase = 0; phase < range.phaseCount; ++phase)
        {
            const int64_t phaseBase =
                static_cast<int64_t>(phase) * static_cast<int64_t>(range.batchesPerPhase);
            const int phaseBatchCount = std::max(
                0,
                static_cast<int>(std::min<int64_t>(
                    static_cast<int64_t>(totalBatches) - phaseBase,
                    range.batchesPerPhase)));
            const int countPerJob = phaseBatchCount / range.jobCount;

            for (int jobIndex = 0; jobIndex < range.jobCount; ++jobIndex)
            {
                const int start = countPerJob * jobIndex;
                int end = countPerJob * (jobIndex + 1);
                if (jobIndex == range.jobCount - 1)
                    end = phaseBatchCount;

                range.startEndByJobAndPhase[RangeSlot(range, jobIndex, phase)] = PackStartEnd(start, end);
            }
        }
    }

    bool GetWorkStealingRange(
        WorkStealingRange& range,
        const int requestedJobIndex,
        int& beginIndex,
        int& endIndex)
    {
        beginIndex = 0;
        endIndex = 0;

        if (range.jobCount <= 0 ||
            range.batchSize <= 0 ||
            range.totalIterationCount <= 0 ||
            requestedJobIndex < 0 ||
            requestedJobIndex >= range.jobCount)
        {
            return false;
        }

        const int jobIndex = requestedJobIndex;
        std::atomic_ref<int> currentPhase(range.currentPhaseByJob[static_cast<size_t>(jobIndex)]);
        int phase = currentPhase.load(std::memory_order_relaxed);

        while (phase < range.phaseCount)
        {
            std::atomic_ref<uint32_t> localSlot(range.startEndByJobAndPhase[RangeSlot(range, jobIndex, phase)]);
            const uint32_t previous = localSlot.fetch_add(1u, std::memory_order_relaxed);
            int rangeStart = PackedStart(previous);
            int rangeEnd = PackedEnd(previous);

            if (rangeStart >= rangeEnd)
            {
                bool stolen = false;
                for (int otherJob = (jobIndex + 1) % range.jobCount;
                     otherJob != jobIndex;
                     otherJob = (otherJob + 1) % range.jobCount)
                {
                    std::atomic_ref<uint32_t> otherSlot(range.startEndByJobAndPhase[RangeSlot(range, otherJob, phase)]);
                    uint32_t observed = otherSlot.load(std::memory_order_relaxed);

                    while (true)
                    {
                        const int otherStart = PackedStart(observed);
                        const int otherEnd = PackedEnd(observed);
                        if (otherStart >= otherEnd)
                            break;

                        const int leaveSize = (otherEnd - otherStart) / 2;
                        const int stolenStart = otherStart + leaveSize;
                        const int stolenEnd = otherEnd;
                        const uint32_t replacement = PackStartEnd(otherStart, stolenStart);
                        if (otherSlot.compare_exchange_weak(
                                observed,
                                replacement,
                                std::memory_order_relaxed,
                                std::memory_order_relaxed))
                        {
                            rangeStart = stolenStart;
                            rangeEnd = stolenEnd;
                            localSlot.store(PackStartEnd(rangeStart + 1, rangeEnd), std::memory_order_relaxed);
                            stolen = true;
                            break;
                        }
                    }

                    if (stolen)
                        break;
                }

                if (!stolen)
                {
                    ++phase;
                    currentPhase.store(phase, std::memory_order_relaxed);
                    continue;
                }
            }

            const int64_t phaseBase =
                static_cast<int64_t>(phase) * static_cast<int64_t>(range.batchesPerPhase);
            const int64_t beginIndex64 =
                (phaseBase + rangeStart) * static_cast<int64_t>(range.batchSize);
            const int64_t endIndex64 = std::min<int64_t>(
                beginIndex64 + range.batchSize,
                range.totalIterationCount);
            beginIndex = static_cast<int>(beginIndex64);
            endIndex = static_cast<int>(endIndex64);
            return beginIndex < endIndex;
        }

        return false;
    }
}
