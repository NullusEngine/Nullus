#include "HeapAllocationCounter.h"
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <vector>
#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace
{
thread_local InputBenchmark::AllocationCounts* activeCounts = nullptr;
void Record(size_t size) noexcept
{
    if (activeCounts) { ++activeCounts->allocations; activeCounts->bytes += size; }
}
void Free(void* pointer) noexcept
{
    if (pointer && activeCounts) ++activeCounts->frees;
    std::free(pointer);
}
void FreeAligned(void* pointer) noexcept
{
    if (pointer && activeCounts) ++activeCounts->frees;
#ifdef _MSC_VER
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
}

void* operator new(size_t size)
{
    if (auto* pointer = std::malloc(size ? size : 1)) { Record(size); return pointer; }
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { Free(pointer); }
void operator delete[](void* pointer) noexcept { Free(pointer); }
void operator delete(void* pointer, size_t) noexcept { Free(pointer); }
void operator delete[](void* pointer, size_t) noexcept { Free(pointer); }
void* operator new(size_t size, const std::nothrow_t&) noexcept
{
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](size_t size, const std::nothrow_t& tag) noexcept { return ::operator new(size, tag); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { Free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { Free(pointer); }
void* operator new(size_t size, std::align_val_t alignment)
{
    void* pointer = nullptr;
#ifdef _MSC_VER
    pointer = _aligned_malloc(size ? size : 1, static_cast<size_t>(alignment));
#else
    if (posix_memalign(&pointer, static_cast<size_t>(alignment), size ? size : 1) != 0) pointer = nullptr;
#endif
    if (!pointer) throw std::bad_alloc();
    Record(size); return pointer;
}
void* operator new[](size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete(void* pointer, std::align_val_t) noexcept { FreeAligned(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { FreeAligned(pointer); }
void operator delete(void* pointer, size_t, std::align_val_t) noexcept { FreeAligned(pointer); }
void operator delete[](void* pointer, size_t, std::align_val_t) noexcept { FreeAligned(pointer); }
void* operator new(size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    try { return ::operator new(size, alignment); } catch (...) { return nullptr; }
}
void* operator new[](size_t size, std::align_val_t alignment, const std::nothrow_t& tag) noexcept { return ::operator new(size, alignment, tag); }
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { FreeAligned(pointer); }
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { FreeAligned(pointer); }

namespace InputBenchmark
{
AllocationScope::AllocationScope(AllocationCounts& counts) noexcept : m_previous(activeCounts) { activeCounts = &counts; }
AllocationScope::~AllocationScope() { activeCounts = m_previous; }

void VerifyAllocationCounter()
{
    AllocationCounts counts;
    {
        AllocationScope scope(counts);
        void* ordinary = ::operator new(23, std::nothrow);
        void* aligned = ::operator new(64, std::align_val_t{64}, std::nothrow);
        if (!ordinary || !aligned) throw std::bad_alloc();
        ::operator delete(ordinary, std::nothrow);
        ::operator delete(aligned, std::align_val_t{64}, std::nothrow);
        std::vector<uint64_t> container;
        container.reserve(7);
        // This verifies interception of STL allocations too, not just direct new.
        if (container.capacity() < 7) throw std::runtime_error("Allocation calibration failed");
    }
    if (counts.allocations != 3 || counts.bytes < 23 + 64 + 7 * sizeof(uint64_t) || counts.frees != 3)
        throw std::runtime_error("C++ allocation counter did not intercept all calibration allocations");
}
}
