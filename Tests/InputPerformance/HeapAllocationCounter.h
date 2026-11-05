#pragma once
#include <cstdint>

namespace InputBenchmark
{
struct AllocationCounts
{
    uint64_t allocations = 0;
    uint64_t bytes = 0;
    uint64_t frees = 0;
};

// Counts C++ allocations on the evaluating thread, after warmup. Reporting,
// clocks and sample storage stay outside the scope. Not an OS heap/leak probe.
class AllocationScope final
{
public:
    explicit AllocationScope(AllocationCounts& counts) noexcept;
    ~AllocationScope();
    AllocationScope(const AllocationScope&) = delete;
    AllocationScope& operator=(const AllocationScope&) = delete;
private:
    AllocationCounts* m_previous;
};
void VerifyAllocationCounter();
}
