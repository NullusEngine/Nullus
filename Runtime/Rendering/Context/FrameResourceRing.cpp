#include "Rendering/Context/FrameResourceRing.h"

#include <algorithm>
#include <thread>
#include <stdexcept>

namespace NLS::Render::Context
{
    namespace
    {
        // Poll slice used while draining GPU fences.
        constexpr auto kDrainPollInterval = std::chrono::microseconds(200);
    }

    FrameResourceRing::FrameResourceRing(const size_t slotCount)
    {
        const size_t resolvedSlotCount = (std::max)(static_cast<size_t>(1u), slotCount);
        m_slots.resize(resolvedSlotCount);
        for (size_t slotIndex = 0u; slotIndex < resolvedSlotCount; ++slotIndex)
            m_slots[slotIndex].slotIndex = slotIndex;
    }

    bool FrameResourceRing::IsSlotReusable(const FrameResourceSlot& slot) const
    {
        if (slot.quarantined)
            return false;
        if (slot.gpuFence != nullptr)
            return slot.gpuFence->IsSignaled();
        // An acquired slot has no retirement fence yet. It stays leased until
        // RetireSlot attaches a fence or explicitly confirms no GPU work.
        return !slot.inFlight;
    }

    void FrameResourceRing::ReleaseSlot(FrameResourceSlot& slot, const ResourceRelease& release)
    {
        if (release)
            release(slot);

        slot.ownedResources.clear();
        slot.streamingDependencyPins.clear();
        slot.retirementPending = false;
        slot.uiTextureRetirementFrameId = 0u;
        slot.gpuFence.reset();
        slot.inFlight = false;
        ++m_reclaimedCount;
    }

    void FrameResourceRing::Grow(const size_t slotCount)
    {
        const size_t previousCount = m_slots.size();
        if (slotCount <= previousCount)
            return;
        m_slots.resize(slotCount);
        for (size_t slotIndex = previousCount; slotIndex < slotCount; ++slotIndex)
            m_slots[slotIndex].slotIndex = slotIndex;
    }

    bool FrameResourceRing::IsRetirementPending(const size_t slotIndex) const
    {
        const auto* slot = PeekSlot(slotIndex);
        return slot != nullptr && slot->retirementPending;
    }

    void FrameResourceRing::EnsureFrameContexts(const size_t frameContextCount)
    {
        Grow(frameContextCount);
        for (size_t slotIndex = m_frameContextCount; slotIndex < frameContextCount; ++slotIndex)
            m_slots[slotIndex].frameContext = std::make_shared<RHI::RHIFrameContext>();
        m_frameContextCount = (std::max)(m_frameContextCount, frameContextCount);
    }

    RHI::RHIFrameContext& FrameResourceRing::GetFrameContext(const size_t slotIndex)
    {
        auto* slot = PeekSlot(slotIndex);
        if (slot == nullptr || slot->frameContext == nullptr)
            throw std::out_of_range("Frame resource slot has no frame context");
        return *slot->frameContext;
    }

    const RHI::RHIFrameContext& FrameResourceRing::GetFrameContext(const size_t slotIndex) const
    {
        const auto* slot = PeekSlot(slotIndex);
        if (slot == nullptr || slot->frameContext == nullptr)
            throw std::out_of_range("Frame resource slot has no frame context");
        return *slot->frameContext;
    }

    bool FrameResourceRing::HasPendingRetirements() const
    {
        return std::any_of(m_slots.begin(), m_slots.end(),
            [](const auto& slot) { return slot.retirementPending; });
    }

    size_t FrameResourceRing::GetOwnedResourceCount() const
    {
        size_t count = 0u;
        for (const auto& slot : m_slots)
            count += slot.ownedResources.size();
        return count;
    }

    FrameResourceSlot* FrameResourceRing::AcquireSlot()
    {
        if (const auto index = FindAvailableSlotIndex(); index.has_value())
            return AcquireSlot(*index);

        // Every slot still has GPU work in flight: the caller must not reuse an
        // allocator, so report starvation instead of silently aliasing memory.
        ++m_starvedCount;
        return nullptr;
    }

    std::optional<size_t> FrameResourceRing::FindAvailableSlotIndex(const size_t reservedTailSlotCount) const
    {
        const size_t availableSlotCount = m_slots.size() - (std::min)(reservedTailSlotCount, m_slots.size());
        for (size_t offset = 0u; offset < m_slots.size(); ++offset)
        {
            const size_t index = (m_nextSlotIndex + offset) % m_slots.size();
            if (index < availableSlotCount && IsSlotReusable(m_slots[index]))
                return index;
        }
        return std::nullopt;
    }

    bool FrameResourceRing::IsSlotAvailable(const size_t slotIndex) const
    {
        if (slotIndex >= m_slots.size())
            return false;

        return IsSlotReusable(m_slots[slotIndex]);
    }

    bool FrameResourceRing::Reset()
    {
        if (std::any_of(m_slots.begin(), m_slots.end(),
            [this](const auto& slot) { return !IsSlotReusable(slot); }))
            return false;
        for (FrameResourceSlot& slot : m_slots)
        {
            slot.gpuFence.reset();
            slot.ownedResources.clear();
            slot.streamingDependencyPins.clear();
            slot.retirementPending = false;
            slot.uiTextureRetirementFrameId = 0u;
            slot.inFlight = false;
        }
        m_nextSlotIndex = 0u;
        return true;
    }

    FrameResourceSlot* FrameResourceRing::AcquireSlot(const size_t slotIndex)
    {
        if (slotIndex >= m_slots.size())
            return nullptr;

        FrameResourceSlot& slot = m_slots[slotIndex];
        if (!IsSlotReusable(slot))
        {
            ++m_starvedCount;
            return nullptr;
        }

        if (slot.inFlight)
            ReleaseSlot(slot, {});

        slot.inFlight = true;
        slot.leaseGeneration = ++m_leaseGeneration;
        m_nextSlotIndex = (slotIndex + 1u) % m_slots.size();
        ++m_acquiredCount;
        return &slot;
    }

    void FrameResourceRing::RetireSlot(
        const size_t slotIndex,
        std::shared_ptr<RHI::RHIFence> gpuFence)
    {
        if (slotIndex >= m_slots.size())
            return;

        FrameResourceSlot& slot = m_slots[slotIndex];
        if (slot.quarantined)
            return;
        slot.gpuFence = std::move(gpuFence);
        if (slot.gpuFence == nullptr)
        {
            ReleaseSlot(slot, {});
            return;
        }
        // Keep the slot marked as owning GPU work until the fence signals.
        slot.inFlight = true;
    }

    size_t FrameResourceRing::ReclaimCompletedSlots(const ResourceRelease& release)
    {
        size_t reclaimed = 0u;
        for (FrameResourceSlot& slot : m_slots)
        {
            if (!slot.inFlight || !IsSlotReusable(slot))
                continue;

            ReleaseSlot(slot, release);
            ++reclaimed;
        }
        return reclaimed;
    }

    bool FrameResourceRing::ReclaimSlot(const size_t slotIndex, const ResourceRelease& release)
    {
        auto* slot = PeekSlot(slotIndex);
        if (slot == nullptr || !IsSlotReusable(*slot) ||
            (!slot->inFlight && !slot->retirementPending && slot->ownedResources.empty()))
            return false;
        ReleaseSlot(*slot, release);
        return true;
    }

    void FrameResourceRing::QuarantineSlot(const size_t slotIndex)
    {
        if (auto* slot = PeekSlot(slotIndex))
        {
            slot->quarantined = true;
            slot->inFlight = true;
        }
    }

    FrameResourceSlot* FrameResourceRing::PeekSlot(const size_t slotIndex)
    {
        return slotIndex < m_slots.size() ? &m_slots[slotIndex] : nullptr;
    }

    const FrameResourceSlot* FrameResourceRing::PeekSlot(const size_t slotIndex) const
    {
        return slotIndex < m_slots.size() ? &m_slots[slotIndex] : nullptr;
    }

    size_t FrameResourceRing::GetInFlightSlotCount() const
    {
        return static_cast<size_t>(std::count_if(
            m_slots.begin(),
            m_slots.end(),
            [](const FrameResourceSlot& slot) { return slot.inFlight; }));
    }

    bool FrameResourceRing::DrainAll(
        const ResourceRelease& release,
        const uint64_t timeoutNanoseconds)
    {
        const bool infiniteWait = timeoutNanoseconds == 0u;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeoutNanoseconds);

        while (GetInFlightSlotCount() > 0u)
        {
            const size_t reclaimed = ReclaimCompletedSlots(release);
            if (reclaimed > 0u)
                continue;

            if (!infiniteWait && std::chrono::steady_clock::now() >= deadline)
                return false;

            std::this_thread::sleep_for(kDrainPollInterval);
        }

        return true;
    }
}
