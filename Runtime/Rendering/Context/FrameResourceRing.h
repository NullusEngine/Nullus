#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/RHI/Core/RHISync.h"

namespace NLS::Render::Context
{
    /**
     * One frame slot's resources. It carries no scheduling state: the frame
     * pipeline is ordered by RenderCommandFence / RhiSubmitEvent, and this slot
     * only answers "is the GPU still using these resources?".
     */
    struct NLS_RENDER_API FrameResourceSlot
    {
        size_t slotIndex = 0u;
        uint64_t leaseGeneration = 0u;
        std::shared_ptr<RHI::RHIFrameContext> frameContext;
        // Signalled when every GPU command issued from this slot has completed.
        std::shared_ptr<RHI::RHIFence> gpuFence;
        // Resources that must outlive the GPU work, released on reset.
        std::vector<std::shared_ptr<void>> ownedResources;
        std::vector<uint64_t> streamingDependencyPins;
        bool retirementPending = false;
        uint64_t uiTextureRetirementFrameId = 0u;
        bool quarantined = false;
        bool inFlight = false;
    };

    /**
     * Fixed-size ring of per-frame resource slots.
     *
     * A slot only becomes reusable after its GPU fence has signalled, so
     * command allocators, transient resources and deferred-release objects are
     * never reset while the GPU can still read them. Unlike the legacy in-flight
     * slot this type owns no published/ready/submitting lifecycle.
     */
    class NLS_RENDER_API FrameResourceRing final
    {
    public:
        using ResourceRelease = std::function<void(FrameResourceSlot&)>;

        explicit FrameResourceRing(size_t slotCount);

        [[nodiscard]] size_t GetSlotCount() const noexcept { return m_slots.size(); }

        // Device-injection fixtures may grow the ring before opening a frame.
        void Grow(size_t slotCount);
        void EnsureFrameContexts(size_t frameContextCount);
        [[nodiscard]] size_t GetFrameContextCount() const noexcept { return m_frameContextCount; }
        [[nodiscard]] RHI::RHIFrameContext& GetFrameContext(size_t slotIndex);
        [[nodiscard]] const RHI::RHIFrameContext& GetFrameContext(size_t slotIndex) const;
        [[nodiscard]] bool IsRetirementPending(size_t slotIndex) const;
        [[nodiscard]] bool HasPendingRetirements() const;
        [[nodiscard]] size_t GetOwnedResourceCount() const;

        /**
         * Claims the next free slot, reusing any slot whose fence has completed.
         * Returns null while every slot still has GPU work in flight.
         */
        FrameResourceSlot* AcquireSlot();
        [[nodiscard]] std::optional<size_t> FindAvailableSlotIndex(size_t reservedTailSlotCount = 0u) const;

        /**
         * Claims a specific slot once its GPU work completed. Used by resize and
         * shutdown paths that need deterministic ordering.
         */
        FrameResourceSlot* AcquireSlot(size_t slotIndex);

        /**
         * Reports whether the resource lease is free or its retirement fence has
         * completed. Querying availability never attaches a fence or changes the
         * lease. RetireSlot is the only normal transfer from CPU to GPU ownership.
         *
         * Returns false for an out-of-range index.
         */
        [[nodiscard]] bool IsSlotAvailable(size_t slotIndex) const;

        /** Rebuild reset is refused while a slot is leased or a GPU fence is pending. */
        bool Reset();

        /**
         * Releases retired slots: resets in-flight state, invokes the release
         * callback for slot-owned resources, and clears the fence.
         * Returns how many slots were reclaimed.
         */
        size_t ReclaimCompletedSlots(const ResourceRelease& release = {});

        bool ReclaimSlot(size_t slotIndex, const ResourceRelease& release = {});
        void QuarantineSlot(size_t slotIndex);

        /**
         * Retires a slot by attaching the fence that will signal when its GPU work
         * finishes. A null fence confirms that no GPU work references this slot
         * and releases its resources immediately.
         */
        void RetireSlot(size_t slotIndex, std::shared_ptr<RHI::RHIFence> gpuFence);

        [[nodiscard]] FrameResourceSlot* PeekSlot(size_t slotIndex);
        [[nodiscard]] const FrameResourceSlot* PeekSlot(size_t slotIndex) const;

        [[nodiscard]] size_t GetInFlightSlotCount() const;
        [[nodiscard]] uint64_t GetAcquiredCount() const noexcept { return m_acquiredCount; }
        [[nodiscard]] uint64_t GetReclaimedCount() const noexcept { return m_reclaimedCount; }
        [[nodiscard]] uint64_t GetStarvedCount() const noexcept { return m_starvedCount; }

        /**
         * Blocks until every slot is reusable, then releases their resources.
         * `timeout` of zero waits indefinitely.
         */
        bool DrainAll(const ResourceRelease& release = {}, uint64_t timeoutNanoseconds = 0u);

    private:
        [[nodiscard]] bool IsSlotReusable(const FrameResourceSlot& slot) const;
        void ReleaseSlot(FrameResourceSlot& slot, const ResourceRelease& release);

        std::vector<FrameResourceSlot> m_slots;
        size_t m_frameContextCount = 0u;
        size_t m_nextSlotIndex = 0u;
        uint64_t m_leaseGeneration = 0u;
        uint64_t m_acquiredCount = 0u;
        uint64_t m_reclaimedCount = 0u;
        uint64_t m_starvedCount = 0u;
    };
}
