#include <gtest/gtest.h>

#include "Threading/ExecutionThreadBudget.h"

namespace NLS::Base::Threading
{
TEST(ExecutionThreadBudgetTests, CountsSharedComputeWorkersOnlyOnce)
{
    for (const uint32_t hardware : {0u, 1u, 2u, 4u, 8u, 16u, 32u, 64u, 256u})
    {
        for (const bool dedicatedRhi : {false, true})
        {
            for (const uint32_t background : {0u, 1u, 2u, 16u, 64u})
            {
                const auto budget = ResolveExecutionPoolBudget(hardware, background, dedicatedRhi);
                EXPECT_EQ(budget.taskGraphWorkerCount, budget.jobsForegroundWorkerCount);
                EXPECT_EQ(budget.jobsWorkerCount, budget.totalPoolWorkerCount);
                EXPECT_EQ(budget.jobsForegroundWorkerCount + budget.jobsBackgroundWorkerCount,
                    budget.totalPoolWorkerCount);
                EXPECT_GE(budget.jobsForegroundWorkerCount, 1u);
                EXPECT_GE(budget.jobsBackgroundWorkerCount, 1u);
                EXPECT_LE(budget.jobsBackgroundWorkerCount, kMaximumBackgroundJobWorkerCount);
                EXPECT_LE(budget.totalPoolWorkerCount, kMaximumPoolWorkerCount);
                const auto available = hardware == 0 ? kFallbackHardwareConcurrency : hardware;
                const auto reserved = dedicatedRhi ? kReservedNamedExecutionLaneCount :
                    kReservedNamedExecutionLaneCountWithoutDedicatedRhi;
                if (available > reserved + 1u)
                    EXPECT_LE(budget.totalPoolWorkerCount, available - reserved);
                else
                    EXPECT_EQ(budget.totalPoolWorkerCount, 2u);
            }
        }
    }
}

TEST(ExecutionThreadBudgetTests, BackgroundOverrideReassignsExistingCapacity)
{
    const auto automatic = ResolveExecutionPoolBudget(20u);
    const auto overridden = ResolveExecutionPoolBudget(20u, 8u);
    EXPECT_EQ(overridden.totalPoolWorkerCount, automatic.totalPoolWorkerCount);
    EXPECT_EQ(overridden.jobsBackgroundWorkerCount, 8u);
    EXPECT_EQ(overridden.jobsForegroundWorkerCount, 9u);
    EXPECT_EQ(overridden.taskGraphWorkerCount, 9u);
}
}
