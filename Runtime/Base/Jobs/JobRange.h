#pragma once

#include "BaseDef.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace NLS::Base::Jobs
{
    struct NLS_BASE_API JobBlockRange
    {
        size_t startIndex = 0u;
        size_t rangeSize = 0u;
        size_t rangesTotal = 0u;
    };

    NLS_BASE_API int CalculateJobCountWithMinIndicesPerJob(
        int arrayLength,
        int minimumIndicesPerJob,
        int workerThreads);
    NLS_BASE_API int ConfigureBlockRanges(
        JobBlockRange* blockRanges,
        int arrayLength,
        int blockRangeCount);
    NLS_BASE_API int ConfigureBlockRangesWithMinIndicesPerJob(
        JobBlockRange* blockRanges,
        int arrayLength,
        int minimumIndicesPerJob,
        int workerThreads);

    struct NLS_BASE_API WorkStealingRange
    {
        int batchSize = 1;
        int jobCount = 0;
        int totalIterationCount = 0;
        int phaseCount = 1;
        int batchesPerPhase = 0;
        std::vector<uint32_t> startEndByJobAndPhase;
        std::vector<int> currentPhaseByJob;
    };

    NLS_BASE_API void InitializeWorkStealingRange(
        WorkStealingRange& range,
        int iterationCount,
        int batchSize,
        int jobCount);
    NLS_BASE_API bool GetWorkStealingRange(
        WorkStealingRange& range,
        int jobIndex,
        int& beginIndex,
        int& endIndex);
}
