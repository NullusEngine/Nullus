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
