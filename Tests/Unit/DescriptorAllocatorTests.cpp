#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <cstdint>
#include <thread>
#include <vector>

#include "Rendering/RHI/Utils/DescriptorAllocator/DescriptorAllocator.h"

TEST(DescriptorAllocatorTests, PersistentAllocationsAreSafeUnderConcurrentCreateAndRelease)
{
    auto allocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(128u);
    ASSERT_NE(allocator, nullptr);

    constexpr uint32_t threadCount = 8u;
    constexpr uint32_t iterationsPerThread = 2000u;
    constexpr uint32_t releaseBatchSize = 16u;

    std::barrier startBarrier(static_cast<std::ptrdiff_t>(threadCount));
    std::atomic<uint32_t> invalidAllocationCount{ 0u };
    std::atomic<uint64_t> releasedDescriptorCount{ 0u };
    std::vector<std::thread> workers;
    workers.reserve(threadCount);

    for (uint32_t threadIndex = 0u; threadIndex < threadCount; ++threadIndex)
    {
        workers.emplace_back([&, threadIndex]()
        {
            std::vector<NLS::Render::RHI::DescriptorAllocation> pendingReleases;
            pendingReleases.reserve(releaseBatchSize);
            startBarrier.arrive_and_wait();

            for (uint32_t iteration = 0u; iteration < iterationsPerThread; ++iteration)
            {
                NLS::Render::RHI::DescriptorAllocationRequest request;
                request.count = 1u + ((threadIndex + iteration) % 4u);
                request.lifetime = NLS::Render::RHI::DescriptorAllocationLifetime::Persistent;
                request.frameIndex = iteration;
                request.debugName = "ConcurrentPersistentDescriptor";

                auto allocation = allocator->Allocate(request);
                if (!allocation.IsValid())
                {
                    ++invalidAllocationCount;
                    continue;
                }

                pendingReleases.push_back(allocation);
                if (pendingReleases.size() >= releaseBatchSize)
                {
                    for (const auto& pendingRelease : pendingReleases)
                    {
                        releasedDescriptorCount += pendingRelease.count;
                        allocator->Release(pendingRelease);
                    }
                    pendingReleases.clear();
                }
            }

            for (const auto& pendingRelease : pendingReleases)
            {
                releasedDescriptorCount += pendingRelease.count;
                allocator->Release(pendingRelease);
            }
        });
    }

    for (auto& worker : workers)
    {
        worker.join();
    }

    const auto stats = allocator->GetStats();
    EXPECT_EQ(invalidAllocationCount.load(), 0u);
    EXPECT_EQ(stats.persistentUsed, 0u);
    EXPECT_EQ(stats.persistentReleased, releasedDescriptorCount.load());
}

TEST(DescriptorAllocatorTests, AllocateBatchReservesTransientDescriptorsAsOneShortLivedGroup)
{
    auto allocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(128u);
    ASSERT_NE(allocator, nullptr);
    allocator->BeginFrame(12u);

    std::vector<NLS::Render::RHI::DescriptorAllocationRequest> requests(3u);
    requests[0].count = 2u;
    requests[0].frameIndex = 12u;
    requests[0].debugName = "BatchA";
    requests[1].count = 3u;
    requests[1].frameIndex = 12u;
    requests[1].debugName = "BatchB";
    requests[2].count = 1u;
    requests[2].frameIndex = 12u;
    requests[2].debugName = "BatchC";

    const auto batch = allocator->AllocateBatch(requests);

    ASSERT_TRUE(batch.allSucceeded);
    ASSERT_EQ(batch.allocations.size(), requests.size());
    EXPECT_EQ(batch.totalRequested, 6u);
    EXPECT_EQ(batch.totalAllocated, 6u);
    EXPECT_EQ(batch.allocations[0].offset, 0u);
    EXPECT_EQ(batch.allocations[1].offset, 2u);
    EXPECT_EQ(batch.allocations[2].offset, 5u);
    EXPECT_EQ(batch.allocations[1].debugName, "BatchB");

    const auto stats = allocator->GetStats();
    EXPECT_EQ(stats.transientUsed, 6u);
    EXPECT_EQ(stats.allocationFailures, 0u);
}

TEST(DescriptorAllocatorTests, DescriptorRangeAllocatorProvidesBoundedPolicyForShaderVisibleHeaps)
{
    NLS::Render::RHI::DescriptorRangeAllocatorDesc desc;
    desc.transientCapacity = 8u;
    desc.persistentCapacity = 5u;
    desc.boundPersistentCapacity = true;
    desc.debugName = "ShaderVisiblePolicy";

    NLS::Render::RHI::DescriptorRangeAllocator allocator(desc);

    NLS::Render::RHI::DescriptorAllocationRequest request;
    request.lifetime = NLS::Render::RHI::DescriptorAllocationLifetime::Persistent;
    request.count = 3u;
    request.debugName = "FirstTable";
    const auto first = allocator.Allocate(request);
    ASSERT_TRUE(first.IsValid());
    EXPECT_EQ(first.offset, 0u);

    request.count = 2u;
    request.debugName = "SecondTable";
    const auto second = allocator.Allocate(request);
    ASSERT_TRUE(second.IsValid());
    EXPECT_EQ(second.offset, 3u);

    request.count = 1u;
    request.debugName = "TooMany";
    const auto overflow = allocator.Allocate(request);
    EXPECT_FALSE(overflow.IsValid());

    auto stats = allocator.GetStats();
    EXPECT_EQ(stats.persistentCapacity, 5u);
    EXPECT_EQ(stats.persistentUsed, 5u);
    EXPECT_EQ(stats.persistentPeak, 5u);
    EXPECT_EQ(stats.allocationFailures, 1u);

    allocator.Release(first);
    request.count = 2u;
    request.debugName = "ReusedTable";
    const auto reused = allocator.Allocate(request);
    ASSERT_TRUE(reused.IsValid());
    EXPECT_EQ(reused.offset, 0u);

    stats = allocator.GetStats();
    EXPECT_EQ(stats.persistentUsed, 4u);
    EXPECT_EQ(stats.persistentReleased, 3u);
}
