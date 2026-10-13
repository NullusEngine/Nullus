#pragma once

#include <algorithm>
#include <cstdint>

namespace NLS::Base::Threading
{
    inline constexpr uint32_t kFallbackHardwareConcurrency = 4u;
    // Main/Game, Render, and (in DedicatedThread mode) RHI each own a dedicated
    // lane outside the pools.
    inline constexpr uint32_t kReservedNamedExecutionLaneCount = 3u;
    // Tasks mode runs the logical RHI lane on pool workers, so only the game and
    // render lanes stay outside the pools.
    inline constexpr uint32_t kReservedNamedExecutionLaneCountWithoutDedicatedRhi = 2u;
    inline constexpr uint32_t kMaximumPoolWorkerCount = 31u;
    inline constexpr uint32_t kMaximumTaskGraphWorkerCount = 12u;
    inline constexpr uint32_t kMaximumBackgroundJobWorkerCount = 16u;

    struct ExecutionPoolBudget
    {
        uint32_t totalPoolWorkerCount = 2u;
        uint32_t taskGraphWorkerCount = 1u;
        uint32_t jobsWorkerCount = 2u;
        uint32_t jobsForegroundWorkerCount = 1u;
        uint32_t jobsBackgroundWorkerCount = 1u;
    };

    /**
     * Splits the hardware lanes that remain after the named execution lanes
     * between the rendering TaskGraph and the general Jobs pools. Both systems
     * resolve the same hardware concurrency, so neither one can over-subscribe
     * the machine on its own.
     *
     * `reserveDedicatedRhiLane` must be false when the RHI lane runs on
     * TaskGraph workers (RhiThreadMode::Tasks or None); otherwise the pool is
     * undersized by one lane and the tasks lane loses the worker it needs.
     */
    inline ExecutionPoolBudget ResolveExecutionPoolBudget(
        uint32_t hardwareConcurrency,
        uint32_t requestedBackgroundJobWorkerCount = 0u,
        bool reserveDedicatedRhiLane = true)
    {
        const uint32_t availableConcurrency = hardwareConcurrency == 0u
            ? kFallbackHardwareConcurrency
            : hardwareConcurrency;

        const uint32_t reservedNamedLanes = reserveDedicatedRhiLane
            ? kReservedNamedExecutionLaneCount
            : kReservedNamedExecutionLaneCountWithoutDedicatedRhi;

        ExecutionPoolBudget budget;
        budget.totalPoolWorkerCount = std::min(
            kMaximumPoolWorkerCount,
            (std::max)(
                2u,
                availableConcurrency > reservedNamedLanes
                    ? availableConcurrency - reservedNamedLanes
                    : 2u));
        budget.taskGraphWorkerCount = std::min(
            kMaximumTaskGraphWorkerCount,
            (std::max)(1u, budget.totalPoolWorkerCount / 2u));
        budget.jobsWorkerCount = (std::max)(
            2u,
            budget.totalPoolWorkerCount - budget.taskGraphWorkerCount);
        // Tiny machines cannot satisfy both pool minimums inside
        // hardware - reserved; report the effective total so callers can reason
        // about the real thread count instead of the hardware-derived target.
        budget.totalPoolWorkerCount =
            budget.taskGraphWorkerCount + budget.jobsWorkerCount;

        const uint32_t automaticBackgroundWorkerCount =
            (std::max)(1u, budget.jobsWorkerCount / 3u);
        budget.jobsBackgroundWorkerCount = std::min(
            kMaximumBackgroundJobWorkerCount,
            std::min(
                budget.jobsWorkerCount - 1u,
                requestedBackgroundJobWorkerCount != 0u
                    ? requestedBackgroundJobWorkerCount
                    : automaticBackgroundWorkerCount));
        budget.jobsForegroundWorkerCount =
            budget.jobsWorkerCount - budget.jobsBackgroundWorkerCount;
        return budget;
    }
}
